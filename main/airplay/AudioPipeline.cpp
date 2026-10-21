#include "AudioPipeline.h"

#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include "AirPlayCrypto.h"
#include "AudioI2s.h"
#include "UiMusicPlayer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace airplay {

namespace {

constexpr const char* TAG = "AirPlayAudio";
constexpr uint32_t kNtpEpochOffset = 2208988800UL;
constexpr size_t kPcmBufferSamples = kMaxDecodedSamplesPerChannel * 2;
constexpr size_t kPlayBufferSamples = kMaxSamplesPerChannel * 2;
// Track changes and seeks restart with a new RTP timestamp base. A short
// rebuffer resumes audio quickly, while the normal 1.5 s prebuffer remains
// in place for the initial connection. Even the short rebuffer needs a
// cushion: anchoring on one or two frames leaves the pipeline with no
// slack, so every scheduling hiccup turns into an audible gap.
constexpr uint32_t kFastRebufferFrames = 10;
// A stream that keeps trickling in one frame at a time never builds a
// cushion. When the buffer stays this thin for kThinRebufferTriggerUs,
// rebuild a real margin instead of playing every frame on arrival.
constexpr size_t kThinBufferSlots = 6;
constexpr uint32_t kThinRebufferFrames = 16;
constexpr int64_t kThinRebufferTriggerUs = 500000;
// The RTP thread runs above the playback loop and LVGL on the same core.
// Cap how long it may run without yielding: a burst of queued packets would
// otherwise leave the audio thread unscheduled for hundreds of milliseconds.
constexpr int64_t kRtpMaxBurstUs = 20000;
// Leave 3 dB of headroom: full-scale, heavily compressed material (rock is a
// good example) otherwise clips the DAC/amplifier, and this also keeps the
// midrange no louder than the original linear volume mapping.
constexpr float kOutputHeadroom = 0.7079458f; // -3 dB

uint32_t readBe32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint16_t readBe16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

void writeNtpTimestamp(uint8_t* out, int64_t localUs)
{
    uint64_t seconds = static_cast<uint64_t>(localUs / 1000000LL) +
                       kNtpEpochOffset;
    uint64_t fraction = static_cast<uint64_t>(
        ((localUs % 1000000LL) << 32) / 1000000LL);
    out[0] = static_cast<uint8_t>(seconds >> 24);
    out[1] = static_cast<uint8_t>(seconds >> 16);
    out[2] = static_cast<uint8_t>(seconds >> 8);
    out[3] = static_cast<uint8_t>(seconds);
    out[4] = static_cast<uint8_t>(fraction >> 24);
    out[5] = static_cast<uint8_t>(fraction >> 16);
    out[6] = static_cast<uint8_t>(fraction >> 8);
    out[7] = static_cast<uint8_t>(fraction);
}

} // namespace

AudioPipeline::~AudioPipeline()
{
    stop();
    heap_caps_free(_rtpBuffer);
    heap_caps_free(_decryptBuffer);
    heap_caps_free(_pcmBuffer);
    heap_caps_free(_playBuffer);
    _rtpBuffer = nullptr;
    _decryptBuffer = nullptr;
    _pcmBuffer = nullptr;
    _playBuffer = nullptr;
}

esp_err_t AudioPipeline::init(AudioI2s* audioI2s)
{
    _i2s = audioI2s;

    _rtpBuffer = static_cast<uint8_t*>(
        heap_caps_malloc(kMaxRtpPacketSize,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    _decryptBuffer = static_cast<uint8_t*>(
        heap_caps_malloc(kMaxRtpPacketSize,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    _pcmBuffer = static_cast<int16_t*>(
        heap_caps_malloc(kPcmBufferSamples * sizeof(int16_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    _playBuffer = static_cast<int16_t*>(
        heap_caps_malloc(kPlayBufferSamples * sizeof(int16_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (_rtpBuffer == nullptr || _decryptBuffer == nullptr ||
        _pcmBuffer == nullptr || _playBuffer == nullptr) {
        ESP_LOGE(TAG, "Audio buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    if (!_jitter.init()) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool AudioPipeline::configure(const SdpInfo& sdp)
{
    if (!sdp.isValid()) {
        ESP_LOGE(TAG, "Rejecting invalid SDP codec=%s rate=%d ch=%d bits=%d",
                 sdp.codec.c_str(), sdp.sampleRate, sdp.channels,
                 sdp.bitsPerSample);
        return false;
    }

    stopInternal(true);

    _sdp = sdp;
    _jitter.flush();
    _needAnchor = true;
    _fastRebuffer.store(false);
    _deepRebuffer.store(false);
    _lastPacketTimestampValid = false;
    _underrunStartUs = 0;
    _thinStartUs = 0;
    _inUnderrun = false;
    _paused.store(false);
    _datagramCount = 0;
    _invalidPackets = 0;
    _seqGaps = 0;
    _resendRequests = 0;
    _seqValid = false;
    _lastSeq = 0;
    _lastResendUs = 0;
    _controlPeerIp = 0;
    _controlPeerPort = 0;
    _timingPeerPort = 0;
    _senderSsrc = 0;
    _firstSequence = 0;
    _highestSequence = 0;
    _validPackets = 0;
    _rtcpReports = 0;
    _highestSequenceValid = false;
    _lastRtcpUs = 0;
    _lastDatagramUs = 0;
    _timingReplies = 0;
    _timingRequests = 0;
    _packetCount = 0;
    _frameCount = 0;
    _diagPackets = 0;
    _decodeErrors = 0;
    _lateDrops = 0;
    _underruns = 0;

    AirPlayCrypto::instance().destroyKey(_aesKey);
    if (_sdp.encrypted) {
        if (_sdp.aesKey.size() < 16 || _sdp.aesIv.size() < 16) {
            ESP_LOGE(TAG, "Encrypted stream without a valid AES key/iv");
            return false;
        }
        _aesKey = AirPlayCrypto::instance().importAesKey(
            _sdp.aesKey.data(), 16);
        if (_aesKey == 0) {
            return false;
        }
    }

    if (!_decoder.open(_sdp)) {
        return false;
    }

    uint32_t frames = (_sdp.sampleRate * kPrebufferMs / 1000) /
                      static_cast<uint32_t>(_sdp.frameLength);
    if (frames < 2) {
        frames = 2;
    }
    if (frames > kPcmJitterSlots / 2) {
        frames = kPcmJitterSlots / 2;
    }
    _prebufferFrames = frames;

    ESP_LOGI(TAG, "Configured %s %d Hz %d ch %d bit, frame=%d, prebuffer=%u frames, encryption=%d",
             _sdp.codec.c_str(), _sdp.sampleRate, _sdp.channels,
             _sdp.bitsPerSample, _sdp.frameLength,
             static_cast<unsigned>(_prebufferFrames),
             _sdp.encrypted ? 1 : 0);
    return true;
}

bool AudioPipeline::openPorts(uint16_t& audioPort, uint16_t& controlPort,
                              uint16_t& timingPort)
{
    // A repeated SETUP must not leave the previous RTP/playback tasks
    // running on sockets that are about to be replaced. Keep the codec and
    // AES key alive: RECORD will restart playback for the same session.
    stopInternal(false);

    if (!openUdpSocket(_audioFd, _audioPort) ||
        !openUdpSocket(_controlFd, _controlPort) ||
        !openUdpSocket(_timingFd, _timingPort)) {
        closePorts();
        return false;
    }

    int controlFlags = fcntl(_controlFd, F_GETFL, 0);
    if (controlFlags >= 0) {
        fcntl(_controlFd, F_SETFL, controlFlags | O_NONBLOCK);
    }

    audioPort = _audioPort;
    controlPort = _controlPort;
    timingPort = _timingPort;

    ESP_LOGI(TAG, "UDP ports: audio=%u control=%u timing=%u",
             static_cast<unsigned>(_audioPort),
             static_cast<unsigned>(_controlPort),
             static_cast<unsigned>(_timingPort));
    return true;
}

void AudioPipeline::start()
{
    if (_running.load()) {
        return;
    }
    if (_audioFd < 0 || _timingFd < 0) {
        ESP_LOGE(TAG, "start() called without openPorts()");
        return;
    }

    constexpr size_t kMinInternalHeapForThreads = 20 * 1024;
    constexpr size_t kMinLargestBlockForThreads = 8 * 1024;
    size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (internalFree < kMinInternalHeapForThreads ||
        largestBlock < kMinLargestBlockForThreads) {
        ESP_LOGE(TAG,
                 "Not enough internal RAM for audio threads: free=%u largest=%u",
                 static_cast<unsigned>(internalFree),
                 static_cast<unsigned>(largestBlock));
        return;
    }

    _running.store(true);
    _needAnchor = true;
    _fastRebuffer.store(false);
    _deepRebuffer.store(false);
    _lastPacketTimestampValid = false;
    _underrunStartUs = 0;
    _thinStartUs = 0;
    _inUnderrun = false;
    _paused.store(false);
    _lastStatsUs = esp_timer_get_time();

    if (_taskExitSem == nullptr) {
        _taskExitSem = xSemaphoreCreateCounting(3, 0);
        if (_taskExitSem == nullptr) {
            _running.store(false);
            ESP_LOGE(TAG, "Failed to create the audio task exit semaphore");
            return;
        }
    }
    // Drop tokens left by a previous run so joinWorkerTasks() below cannot
    // return before the tasks it is waiting for have exited.
    while (xSemaphoreTake(_taskExitSem, 0) == pdTRUE) {
    }

    ESP_LOGI(TAG, "heap before audio tasks: internal=%u largest=%u psram=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    // Create the tasks with the FreeRTOS API: it reports failures through a
    // return value, so a sender that arrived while the internal heap was
    // exhausted no longer aborts the whole device.
    bool created = true;
    if (xTaskCreatePinnedToCore(&AudioPipeline::rtpTaskEntry, "airplay_rtp",
                                8192, this, 11, &_rtpTask, 1) != pdPASS) {
        _rtpTask = nullptr;
        created = false;
    }
    if (xTaskCreatePinnedToCore(&AudioPipeline::playbackTaskEntry,
                                "airplay_play", 3072, this, 8, &_playTask,
                                1) != pdPASS) {
        _playTask = nullptr;
        created = false;
    }
    if (xTaskCreatePinnedToCore(&AudioPipeline::timingTaskEntry,
                                "airplay_time", 3072, this, 6, &_timingTask,
                                0) != pdPASS) {
        _timingTask = nullptr;
        created = false;
    }

    if (!created) {
        ESP_LOGE(TAG,
                 "Audio task creation failed: internal=%u largest=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        // Let whichever tasks did start run out and exit.
        stopInternal(false);
        return;
    }

    ESP_LOGI(TAG, "AirPlay audio started: %d Hz, %d ch, audio port %u",
             _sdp.sampleRate, _sdp.channels,
             static_cast<unsigned>(_audioPort));
}

void AudioPipeline::rtpTaskEntry(void* arg)
{
    auto* self = static_cast<AudioPipeline*>(arg);
    self->rtpLoop();
    if (self->_taskExitSem != nullptr) {
        xSemaphoreGive(self->_taskExitSem);
    }
    vTaskDelete(nullptr);
}

void AudioPipeline::playbackTaskEntry(void* arg)
{
    auto* self = static_cast<AudioPipeline*>(arg);
    self->playbackLoop();
    if (self->_taskExitSem != nullptr) {
        xSemaphoreGive(self->_taskExitSem);
    }
    vTaskDelete(nullptr);
}

void AudioPipeline::timingTaskEntry(void* arg)
{
    auto* self = static_cast<AudioPipeline*>(arg);
    self->timingLoop();
    if (self->_taskExitSem != nullptr) {
        xSemaphoreGive(self->_taskExitSem);
    }
    vTaskDelete(nullptr);
}

void AudioPipeline::joinWorkerTasks(uint32_t timeoutMs)
{
    int pending = 0;
    if (_rtpTask != nullptr) pending++;
    if (_playTask != nullptr) pending++;
    if (_timingTask != nullptr) pending++;

    for (int i = 0; i < pending; ++i) {
        if (_taskExitSem != nullptr) {
            xSemaphoreTake(_taskExitSem, pdMS_TO_TICKS(timeoutMs));
        }
    }

    _rtpTask = nullptr;
    _playTask = nullptr;
    _timingTask = nullptr;
}

void AudioPipeline::stop()
{
    stopInternal(true);
}

void AudioPipeline::stopInternal(bool releaseResources)
{
    bool wasRunning = _running.exchange(false);
    closePorts();

    joinWorkerTasks(3000);

    _jitter.flush();
    {
        std::lock_guard<std::mutex> lock(_decoderMutex);
        _decoder.reset();
        if (releaseResources) {
            _decoder.close();
        }
    }
    if (_i2s != nullptr) {
        _i2s->clearI2sRingbuffer();
    }
    _needAnchor = true;
    _fastRebuffer.store(false);
    _lastPacketTimestampValid = false;
    _underrunStartUs = 0;

    if (releaseResources) {
        AirPlayCrypto::instance().destroyKey(_aesKey);
    }

    if (wasRunning) {
        ESP_LOGI(TAG, "AirPlay audio stopped (packets=%u decode_err=%u late=%u underruns=%u)",
                 static_cast<unsigned>(_packetCount),
                 static_cast<unsigned>(_decodeErrors),
                 static_cast<unsigned>(_lateDrops),
                 static_cast<unsigned>(_underruns));
    }
}

void AudioPipeline::flush(bool clearI2s)
{
    _jitter.flush();
    {
        std::lock_guard<std::mutex> lock(_decoderMutex);
        _decoder.reset();
    }
    _needAnchor = true;
    _fastRebuffer.store(true);
    _deepRebuffer.store(false);
    _lastPacketTimestampValid = false;
    _underrunStartUs = 0;
    _thinStartUs = 0;
    _inUnderrun = false;
    _seqValid = false;
    _lastSeq = 0;
    if (clearI2s && _i2s != nullptr) {
        _i2s->clearI2sRingbuffer();
    }
    ESP_LOGI(TAG, "Audio flushed%s", clearI2s ? "" : " (I2S tail kept)");
}

void AudioPipeline::pause()
{
    _paused.store(true);
    flush();
    ESP_LOGI(TAG, "Audio paused");
}

void AudioPipeline::resume()
{
    _paused.store(false);
    _needAnchor = true;
    ESP_LOGI(TAG, "Audio resumed");
}

void AudioPipeline::setVisualizer(UiMusicPlayer* ui)
{
    _visualizer.store(ui);
}

bool AudioPipeline::isRtpStalled(uint32_t timeoutMs) const
{
    int64_t last = _lastDatagramUs.load();
    return last != 0 &&
           (esp_timer_get_time() - last) >
               static_cast<int64_t>(timeoutMs) * 1000;
}

bool AudioPipeline::isPlaybackReady() const
{
    if (!_running.load() || _paused.load()) {
        // No PCM is being pushed to I2S, so a cover-art decode cannot
        // interrupt audio output.
        return true;
    }
    return !_needAnchor.load() &&
           _jitter.size() >= kArtworkMinBufferSlots;
}

void AudioPipeline::setControlPeer(uint32_t ipv4, uint16_t controlPort,
                                   uint16_t timingPort)
{
    _controlPeerIp = ipv4;
    _controlPeerPort = controlPort;
    _timingPeerPort = timingPort;
    ESP_LOGI(TAG, "Peer set to %u.%u.%u.%u control=%u timing=%u",
             static_cast<unsigned>(ipv4 & 0xff),
             static_cast<unsigned>((ipv4 >> 8) & 0xff),
             static_cast<unsigned>((ipv4 >> 16) & 0xff),
             static_cast<unsigned>((ipv4 >> 24) & 0xff),
             static_cast<unsigned>(controlPort),
             static_cast<unsigned>(timingPort));
}

void AudioPipeline::sendTimingRequest()
{
    if (_timingFd < 0 || _controlPeerIp == 0 || _timingPeerPort == 0) {
        return;
    }

    // AirPlay timing request: 0x80 0xd2, sequence, filler, origin,
    // receive and transmit NTP timestamps. Apple's sender replies with 0xd3.
    uint8_t request[32] = {};
    request[0] = 0x80;
    request[1] = 0xD2;
    request[2] = 0x00;
    request[3] = 0x07;

    struct sockaddr_in peer = {};
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = _controlPeerIp;
    peer.sin_port = htons(_timingPeerPort);
    int sent = sendto(_timingFd, request, sizeof(request), 0,
                      reinterpret_cast<struct sockaddr*>(&peer),
                      sizeof(peer));
    if (sent == static_cast<int>(sizeof(request))) {
        _timingRequests++;
        if (_timingRequests <= 3) {
            ESP_LOGD(TAG, "Timing request sent (%u)",
                     static_cast<unsigned>(_timingRequests));
        }
    } else {
        ESP_LOGW(TAG, "Timing request sendto failed: errno=%d", errno);
    }
}

void AudioPipeline::requestResend(uint16_t firstSequence, uint16_t count)
{
    if (_controlFd < 0 || _controlPeerIp == 0 || _controlPeerPort == 0) {
        return;
    }
    if (count == 0) {
        return;
    }
    if (count > 128) {
        count = 128;
    }

    int64_t now = esp_timer_get_time();
    if (_lastResendUs != 0 && (now - _lastResendUs) < 20000) {
        return;
    }
    _lastResendUs = now;

    // AirPlay/RAOP retransmission request (not a standard RTCP NACK).
    uint8_t request[8] = {
        0x80,
        0xD5,
        0x00,
        0x01,
        static_cast<uint8_t>(firstSequence >> 8),
        static_cast<uint8_t>(firstSequence),
        static_cast<uint8_t>(count >> 8),
        static_cast<uint8_t>(count),
    };

    struct sockaddr_in peer = {};
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = _controlPeerIp;
    peer.sin_port = htons(_controlPeerPort);
    int sent = sendto(_controlFd, request, sizeof(request), 0,
                      reinterpret_cast<struct sockaddr*>(&peer),
                      sizeof(peer));
    if (sent == static_cast<int>(sizeof(request))) {
        _resendRequests++;
    } else {
        ESP_LOGW(TAG, "Resend request sendto failed: errno=%d", errno);
    }
}

void AudioPipeline::sendRtcpReceiverReport()
{
    if (_controlFd < 0 || _controlPeerIp == 0 || _controlPeerPort == 0 ||
        !_highestSequenceValid) {
        return;
    }

    uint32_t expected = _highestSequence - _firstSequence + 1;
    uint32_t lost = expected > _validPackets ? expected - _validPackets : 0;
    if (lost > 0x00FFFFFF) {
        lost = 0x00FFFFFF;
    }

    // RTCP receiver report with one report block (32 bytes).
    uint8_t report[32] = {};
    report[0] = 0x81; // V=2, P=0, RC=1 (one report block)
    report[1] = 201;
    report[3] = 7; // (32 / 4) - 1
    uint32_t receiverSsrc = 0x41505258; // "APRX"
    report[4] = static_cast<uint8_t>(receiverSsrc >> 24);
    report[5] = static_cast<uint8_t>(receiverSsrc >> 16);
    report[6] = static_cast<uint8_t>(receiverSsrc >> 8);
    report[7] = static_cast<uint8_t>(receiverSsrc);
    report[8] = static_cast<uint8_t>(_senderSsrc >> 24);
    report[9] = static_cast<uint8_t>(_senderSsrc >> 16);
    report[10] = static_cast<uint8_t>(_senderSsrc >> 8);
    report[11] = static_cast<uint8_t>(_senderSsrc);
    report[12] = 0; // fraction lost
    report[13] = static_cast<uint8_t>(lost >> 16);
    report[14] = static_cast<uint8_t>(lost >> 8);
    report[15] = static_cast<uint8_t>(lost);
    report[16] = static_cast<uint8_t>(_highestSequence >> 24);
    report[17] = static_cast<uint8_t>(_highestSequence >> 16);
    report[18] = static_cast<uint8_t>(_highestSequence >> 8);
    report[19] = static_cast<uint8_t>(_highestSequence);
    // Jitter, LSR and DLSR stay zero: the sender only needs to see that the
    // receiver is alive and roughly how many packets were lost.

    struct sockaddr_in peer = {};
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = _controlPeerIp;
    peer.sin_port = htons(_controlPeerPort);
    int sent = sendto(_controlFd, report, sizeof(report), 0,
                      reinterpret_cast<struct sockaddr*>(&peer),
                      sizeof(peer));
    if (sent == static_cast<int>(sizeof(report))) {
        _rtcpReports++;
        if (_rtcpReports <= 2) {
            ESP_LOGI(TAG, "RTCP receiver report sent (%u)",
                     static_cast<unsigned>(_rtcpReports));
        }
    } else {
        ESP_LOGW(TAG, "RTCP RR sendto failed: errno=%d", errno);
    }
}

void AudioPipeline::setVolumeDb(float db)
{
    if (db > 0.0f) {
        db = 0.0f;
    }

    _volumeDb.store(db);

    // AirPlay senders use 0 dB for maximum and -30 dB for minimum. Mapping
    // that range linearly to gain leaves the bottom of the phone slider too
    // loud on this board, so normalise it and use a square-law curve; -30 dB
    // and below is silence.
    int32_t gainQ15 = 0;
    if (db > -30.0f) {
        const float normalized = (db + 30.0f) / 30.0f;
        const float gain = kOutputHeadroom * normalized * normalized;
        gainQ15 = static_cast<int32_t>(gain * 32768.0f);
    }

    // Apply the gain in the I2S task, not while filling the jitter/ring
    // buffers. Audio already queued for the DAC then changes volume
    // immediately instead of waiting for the buffer to drain.
    if (_i2s != nullptr) {
        _i2s->setOutputVolumeQ15(gainQ15);
    }
}

void AudioPipeline::closePorts()
{
    if (_audioFd >= 0) {
        close(_audioFd);
        _audioFd = -1;
    }
    if (_controlFd >= 0) {
        close(_controlFd);
        _controlFd = -1;
    }
    if (_timingFd >= 0) {
        close(_timingFd);
        _timingFd = -1;
    }
    _audioPort = 0;
    _controlPort = 0;
    _timingPort = 0;
}

bool AudioPipeline::openUdpSocket(int& fd, uint16_t& port)
{
    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket(UDP) failed: errno=%d", errno);
        return false;
    }

    int rcvbuf = 64 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct timeval tv = {};
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind(UDP) failed: errno=%d", errno);
        close(fd);
        fd = -1;
        return false;
    }

    socklen_t addrLen = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr),
                    &addrLen) != 0) {
        ESP_LOGE(TAG, "getsockname(UDP) failed: errno=%d", errno);
        close(fd);
        fd = -1;
        return false;
    }

    port = ntohs(addr.sin_port);
    return true;
}

void AudioPipeline::rtpLoop()
{
    int64_t lastStats = esp_timer_get_time();
    int64_t busySinceUs = 0;

    uint8_t controlPacket[256];
    while (_running.load()) {
        // The sender also writes RTCP sync packets to the control port.
        // Drain them so their pbufs cannot accumulate in the internal heap,
        // but bound the work per audio packet so a sync burst cannot starve
        // RTP reception.
        for (int drained = 0; drained < 8; ++drained) {
            if (recv(_controlFd, controlPacket, sizeof(controlPacket), 0) <=
                0) {
                break;
            }
        }

        int received = recv(_audioFd, _rtpBuffer, kMaxRtpPacketSize, 0);
        if (received > 0) {
            const int64_t startUs = esp_timer_get_time();
            handleRtpPacket(_rtpBuffer, static_cast<size_t>(received));
            const int64_t busyUs = esp_timer_get_time() - startUs;
            if (busyUs > _maxRtpUs) {
                _maxRtpUs = busyUs;
            }
            _rtpBusyUs += busyUs;

            // This thread outranks both the playback loop and LVGL on the
            // same core, so a burst of queued packets used to keep it running
            // for hundreds of ms and left the audio thread unscheduled. Bound
            // the continuous run: the 1.5 s jitter buffer absorbs the pause.
            if (busySinceUs == 0) {
                busySinceUs = startUs;
            }
            if (esp_timer_get_time() - busySinceUs > kRtpMaxBurstUs) {
                vTaskDelay(1);
                busySinceUs = esp_timer_get_time();
            }
        } else {
            busySinceUs = 0;
            if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                errno != EINTR) {
                if (!_running.load()) {
                    break;
                }
                ESP_LOGW(TAG, "RTP recv failed: errno=%d", errno);
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }

        int64_t now = esp_timer_get_time();
        // Keep periodic logging sparse: UART output from the RTP thread can
        // briefly delay packet processing and is audible on some DACs.
        if (now - lastStats >= 10000000) {
            lastStats = now;
            // AirPlay 1 senders do not require RTCP receiver reports;
            // shairport-sync also only sends resend (NACK) requests.
            if (_lastHeapLogUs == 0 || (now - _lastHeapLogUs) >= 30000000) {
                _lastHeapLogUs = now;
                ESP_LOGI(TAG, "heap: internal=%u largest=%u psram=%u",
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
            }
            if (_lastDatagramUs != 0 && (now - _lastDatagramUs) > 5000000) {
                ESP_LOGW(TAG, "No RTP data for %.1f s",
                         static_cast<double>(now - _lastDatagramUs) / 1000000.0);
            }
            if (_packetCount > 0) {
                ESP_LOGI(TAG, "stats: datagrams=%u invalid=%u packets=%u frames=%u seq_gaps=%u resends=%u rtcp=%u timing=%u decode_err=%u late=%u underruns=%u i2s_underruns=%u buffer=%u maxgap=%ums maxsend=%ums maxrtp=%ums rtpbusy=%ums",
                         static_cast<unsigned>(_datagramCount),
                         static_cast<unsigned>(_invalidPackets),
                         static_cast<unsigned>(_packetCount),
                         static_cast<unsigned>(_frameCount),
                         static_cast<unsigned>(_seqGaps),
                         static_cast<unsigned>(_resendRequests),
                         static_cast<unsigned>(_rtcpReports),
                         static_cast<unsigned>(_timingReplies),
                         static_cast<unsigned>(_decodeErrors),
                         static_cast<unsigned>(_lateDrops),
                         static_cast<unsigned>(_underruns),
                         _i2s != nullptr
                             ? static_cast<unsigned>(_i2s->underrunCount())
                             : 0u,
                         static_cast<unsigned>(_jitter.size()),
                         static_cast<unsigned>(_maxPlayGapUs / 1000),
                         static_cast<unsigned>(_maxSendUs / 1000),
                         static_cast<unsigned>(_maxRtpUs / 1000),
                         static_cast<unsigned>(_rtpBusyUs / 1000));
                _maxPlayGapUs = 0;
                _maxSendUs = 0;
                _maxRtpUs = 0;
                _rtpBusyUs = 0;            } else {
                ESP_LOGW(TAG, "No RTP packets received yet on UDP port %u",
                         static_cast<unsigned>(_audioPort));
            }
        }
    }
}

void AudioPipeline::handleRtpPacket(const uint8_t* data, size_t len)
{
    _datagramCount++;
    _lastDatagramUs = esp_timer_get_time();

    if (len < 12) {
        _invalidPackets++;
        return;
    }

    uint8_t version = static_cast<uint8_t>(data[0] >> 6);
    if (version != 2) {
        _invalidPackets++;
        return;
    }

    size_t headerLen = 12 + (static_cast<size_t>(data[0] & 0x0F) * 4);
    if ((data[0] & 0x10) != 0) {
        if (len < headerLen + 4) {
            _invalidPackets++;
            return;
        }
        uint16_t extWords = readBe16(data + headerLen + 2);
        headerLen += 4 + (static_cast<size_t>(extWords) * 4);
    }
    if (len <= headerLen) {
        _invalidPackets++;
        return;
    }
    _validPackets++;

    uint16_t sequence = readBe16(data + 2);
    if (!_highestSequenceValid) {
        _senderSsrc = readBe32(data + 8);
        _firstSequence = sequence;
        _highestSequence = sequence;
        _highestSequenceValid = true;
    } else {
        uint16_t difference = static_cast<uint16_t>(sequence - _highestSequence);
        if (difference != 0 && difference < 0x8000) {
            _highestSequence += difference;
        }
    }
    if (_seqValid) {
        uint16_t difference = static_cast<uint16_t>(sequence - _lastSeq);
        if (difference == 1) {
            _lastSeq = sequence;
        } else if (difference != 0 && difference < 0x8000) {
            // Packets are missing. Ask the sender's control port to
            // retransmit them; the jitter buffer can still accept them
            // because frames are ordered by RTP timestamp.
            _seqGaps++;
            uint16_t expected = static_cast<uint16_t>(_lastSeq + 1);
            requestResend(expected, static_cast<uint16_t>(difference - 1));
            _lastSeq = sequence;
        }
        // difference >= 0x8000 is an old/out-of-order packet: leave the
        // high-water sequence number alone.
    } else {
        _lastSeq = sequence;
        _seqValid = true;
    }

    uint32_t timestamp = readBe32(data + 4);

    // An old retransmission can carry a timestamp far behind the current
    // stream. Do not mistake it for a track change: a recent out-of-order
    // packet has a small backward sequence distance, while a stream restart
    // usually jumps much further back.
    bool outOfOrder = false;
    if (_seqValid) {
        outOfOrder = static_cast<uint16_t>(_lastSeq - sequence) <= 256;
    }

    if (!outOfOrder) {
        if (_lastPacketTimestampValid) {
            int32_t delta =
                static_cast<int32_t>(timestamp - _lastPacketTimestamp);
            int32_t limit = static_cast<int32_t>(_sdp.sampleRate);
            if (limit < 8000) {
                limit = 8000;
            }
            if (delta > limit || delta < -limit) {
                ESP_LOGW(TAG,
                         "RTP timestamp discontinuity %u -> %u (delta=%d); "
                         "re-anchoring",
                         static_cast<unsigned>(_lastPacketTimestamp),
                         static_cast<unsigned>(timestamp), delta);
                flush(false);
                // flush() reset the sequence tracker; re-establish it with
                // the packet that starts the new stream.
                _lastSeq = sequence;
                _seqValid = true;
            }
        }
        _lastPacketTimestamp = timestamp;
        _lastPacketTimestampValid = true;
    }

    const uint8_t* payload = data + headerLen;
    size_t payloadLen = len - headerLen;

    if (!_firstPacketLogged) {
        _firstPacketLogged = true;
        ESP_LOGI(TAG, "First RTP packet: seq=%u ts=%u payload=%u encrypted=%d",
                 static_cast<unsigned>(sequence),
                 static_cast<unsigned>(timestamp),
                 static_cast<unsigned>(payloadLen), _aesKey != 0 ? 1 : 0);
    }

    if (_aesKey != 0 && _sdp.aesIv.size() >= 16) {
        size_t encryptedLen = payloadLen & ~static_cast<size_t>(0x0F);
        if (encryptedLen > 0) {
            size_t plainLen = 0;
            if (!AirPlayCrypto::instance().aesCbcDecrypt(
                    _aesKey, _sdp.aesIv.data(), payload, encryptedLen,
                    _decryptBuffer, kMaxRtpPacketSize, plainLen)) {
                _decodeErrors++;
                if ((_decodeErrors % 100) == 1) {
                    ESP_LOGW(TAG, "AES decrypt failed (%u errors)",
                             static_cast<unsigned>(_decodeErrors));
                }
                return;
            }
            // RAOP AES-CBC leaves the final partial block unencrypted.
            size_t tail = payloadLen - encryptedLen;
            if (tail > 0 && plainLen + tail <= kMaxRtpPacketSize) {
                memcpy(_decryptBuffer + plainLen, payload + encryptedLen, tail);
                plainLen += tail;
            }
            payload = _decryptBuffer;
            payloadLen = plainLen;
        }
    }

    // A single RTP packet can contain more than one ALAC frame. Decode
    // until the whole payload is consumed and advance the RTP timestamp for
    // every frame we enqueue.
    size_t offset = 0;
    uint32_t frameTimestamp = timestamp;
    bool queuedAny = false;

    while (offset < payloadLen) {
        size_t decodedBytes = 0;
        size_t consumedBytes = 0;
        bool decoded = false;
        {
            std::lock_guard<std::mutex> lock(_decoderMutex);
            decoded = _decoder.decode(
                payload + offset, payloadLen - offset, _pcmBuffer,
                kPcmBufferSamples * sizeof(int16_t), decodedBytes,
                consumedBytes);
        }
        if (!decoded) {
            _decodeErrors++;
            break;
        }

        size_t totalSamples = decodedBytes / sizeof(int16_t);
        if (totalSamples == 0 || _sdp.channels <= 0) {
            _invalidPackets++;
            break;
        }
        size_t samplesPerChannel =
            totalSamples / static_cast<size_t>(_sdp.channels);
        if (samplesPerChannel == 0) {
            _invalidPackets++;
            break;
        }

        if (_diagPackets < 3 || (_diagPackets % 5000) == 0) {
            ESP_LOGI(TAG,
                     "RTP seq=%u ts=%u payload=%u decoded=%u consumed=%u samples=%u",
                     static_cast<unsigned>(sequence),
                     static_cast<unsigned>(frameTimestamp),
                     static_cast<unsigned>(payloadLen),
                     static_cast<unsigned>(decodedBytes),
                     static_cast<unsigned>(consumedBytes),
                     static_cast<unsigned>(samplesPerChannel));
        }

        // The decoder may return several ALAC frames worth of PCM in one
        // call. The jitter buffer has a fixed per-slot size, so push it in
        // chunks and advance the RTP timestamp for each chunk.
        size_t pushed = 0;
        while (pushed < samplesPerChannel) {
            size_t chunk = samplesPerChannel - pushed;
            if (chunk > kMaxSamplesPerChannel) {
                chunk = kMaxSamplesPerChannel;
            }
            const int16_t* chunkData =
                _pcmBuffer + (pushed * static_cast<size_t>(_sdp.channels));
            if (!_jitter.push(
                    frameTimestamp + static_cast<uint32_t>(pushed), chunkData,
                    chunk, static_cast<uint16_t>(_sdp.channels))) {
                _lateDrops++;
            } else {
                queuedAny = true;
                _frameCount++;
            }
            pushed += chunk;
        }

        frameTimestamp += static_cast<uint32_t>(samplesPerChannel);
        if (consumedBytes == 0 ||
            consumedBytes > payloadLen - offset) {
            break;
        }
        offset += consumedBytes;
    }

    _diagPackets++;
    if (queuedAny) {
        _packetCount++;
    }
}

void AudioPipeline::playbackLoop()
{
    while (_running.load()) {
        const int64_t loopStartUs = esp_timer_get_time();
        if (_lastPlayLoopUs != 0) {
            const int64_t gap = loopStartUs - _lastPlayLoopUs;
            if (gap > _maxPlayGapUs) {
                _maxPlayGapUs = gap;
            }
        }
        _lastPlayLoopUs = loopStartUs;

        if (_paused.load()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        PcmJitterBuffer::FrameInfo info = {};

        if (_needAnchor) {
            const bool fast = _fastRebuffer.load();
            const bool deep = _deepRebuffer.exchange(false);
            uint32_t targetFrames =
                deep ? kThinRebufferFrames
                     : (fast ? kFastRebufferFrames : _prebufferFrames);
            int64_t timeoutUs = (fast || deep) ? 1000000 : 2000000;
            int64_t start = esp_timer_get_time();
            while (_running.load() && !_paused.load() &&
                   _jitter.size() < targetFrames &&
                   (esp_timer_get_time() - start) < timeoutUs) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            if (!_jitter.peekOldest(info)) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            _anchorRtp = info.timestamp;
            _anchorLocalUs =
                esp_timer_get_time() + (fast ? 20000 : 50000);
            _needAnchor = false;
            _fastRebuffer.store(false);
            ESP_LOGI(TAG, "Playback anchor set, buffer=%u frames%s",
                     static_cast<unsigned>(_jitter.size()),
                     fast ? " (fast rebuffer)"
                          : (deep ? " (rebuilt thin buffer)" : ""));
        }

        auto frameDueUs = [this](uint32_t timestamp) -> int64_t {
            int32_t delta = static_cast<int32_t>(timestamp - _anchorRtp);
            return _anchorLocalUs +
                   (static_cast<int64_t>(delta) * 1000000LL /
                    static_cast<int64_t>(_sdp.sampleRate));
        };

        if (!_jitter.peekOldest(info)) {
            // The I2S task feeds silence on underrun. Writing silence from
            // here would flood the I2S ring (one frame per millisecond,
            // ~1.4 MB/s at 44.1 kHz stereo), force it into DROPPING mode and
            // discard the first frames of the next track.
            if (!_inUnderrun) {
                _inUnderrun = true;
                _underruns++;
            }
            int64_t nowUs = esp_timer_get_time();
            if (_underrunStartUs == 0) {
                _underrunStartUs = nowUs;
            } else if ((nowUs - _underrunStartUs) > 250000) {
                // After a long gap the old anchor makes the next frames
                // overdue, which would drain them as a burst. Re-anchor to
                // the new data instead.
                _needAnchor = true;
                _fastRebuffer.store(true);
                _underrunStartUs = 0;
                ESP_LOGW(TAG, "Playback underrun > 250 ms; re-anchoring");
            }
            // Block on the jitter buffer instead of sleeping for a tick:
            // the wait ends as soon as a packet arrives, and it yields the
            // CPU so IDLE1 keeps resetting the task watchdog.
            _jitter.waitForFrame(20);
            continue;
        }
        _inUnderrun = false;
        _underrunStartUs = 0;

        int64_t nowUs = esp_timer_get_time();

        // A thin buffer cannot absorb any scheduling or network jitter.
        // Rather than playing frame by frame forever, rebuild a margin.
        if (_jitter.size() < kThinBufferSlots) {
            if (_thinStartUs == 0) {
                _thinStartUs = nowUs;
            } else if ((nowUs - _thinStartUs) > kThinRebufferTriggerUs) {
                ESP_LOGW(TAG,
                         "Jitter buffer stayed thin (%u frames); rebuilding %u",
                         static_cast<unsigned>(_jitter.size()),
                         static_cast<unsigned>(kThinRebufferFrames));
                _thinStartUs = 0;
                _deepRebuffer.store(true);
                _needAnchor = true;
                continue;
            }
        } else {
            _thinStartUs = 0;
        }

        int64_t dueUs = frameDueUs(info.timestamp);

        if (nowUs < dueUs) {
            int64_t waitUs = dueUs - nowUs;
            if (waitUs > 200000 &&
                (nowUs - _lastWaitLogUs) > 2000000) {
                _lastWaitLogUs = nowUs;
                ESP_LOGW(TAG,
                         "Playback waiting %.0f ms for ts=%u (anchor=%u, buffer=%u)",
                         static_cast<double>(waitUs) / 1000.0,
                         static_cast<unsigned>(info.timestamp),
                         static_cast<unsigned>(_anchorRtp),
                         static_cast<unsigned>(_jitter.size()));
            }
            if (waitUs > 10000) {
                waitUs = 10000;
            }
            TickType_t ticks = pdMS_TO_TICKS((waitUs + 999) / 1000);
            vTaskDelay(ticks > 0 ? ticks : 1);
            continue;
        }

        // FreeRTOS is configured at 100 Hz, so one tick is 10 ms while an
        // ALAC frame is 8 ms. Drain every frame that is already due at this
        // wakeup instead of releasing exactly one frame per tick; otherwise
        // the playback rate falls to 100 frames/s and the buffer slowly
        // fills until frames are dropped.
        //
        // Emitting one frame also feeds the analyser; the I2S task applies
        // the output volume when the frame is consumed.
        auto emitFrame = [this](const PcmJitterBuffer::FrameInfo& frame) -> bool {
            size_t sampleCount = frame.samplesPerChannel * frame.channels;
            if (sampleCount > kPlayBufferSamples) {
                sampleCount = kPlayBufferSamples;
            }
            if (sampleCount == 0) {
                return true;
            }

            // Feed the spectrum analyser before the output volume stage: the
            // visualiser should show the signal, not the listening level, so
            // turning the volume down (or muting) leaves the bars alone.
            // Queuing is non-blocking: the analyser drops audio when it falls
            // behind.
            UiMusicPlayer* visualizer = _visualizer.load();
            if (visualizer != nullptr && _sdp.channels == 2) {
                visualizer->audioVisual(
                    reinterpret_cast<const uint8_t*>(_playBuffer),
                    sampleCount * sizeof(int16_t));
            }

            if (_i2s == nullptr) {
                return true;
            }

            const int64_t sendStartUs = esp_timer_get_time();
            size_t written = _i2s->sendToI2s(
                reinterpret_cast<const uint8_t*>(_playBuffer),
                sampleCount * sizeof(int16_t));
            const int64_t sendUs = esp_timer_get_time() - sendStartUs;
            if (sendUs > _maxSendUs) {
                _maxSendUs = sendUs;
            }
            if (written == 0) {
                if ((_packetCount % 250) == 0) {
                    ESP_LOGW(TAG, "I2S ring buffer rejected a PCM frame");
                }
                return false;
            }
            return true;
        };

        for (int drained = 0; drained < 8; ++drained) {
            if (!_running.load() || _paused.load()) {
                break;
            }
            if (!_jitter.peekOldest(info)) {
                if (!_inUnderrun) {
                    _inUnderrun = true;
                    _underruns++;
                }
                break;
            }

            int64_t frameDue = frameDueUs(info.timestamp);
            int64_t now = esp_timer_get_time();
            if (now < frameDue) {
                break;
            }

            if (now - frameDue >
                static_cast<int64_t>(kMaxLateFrameMs) * 1000LL) {
                _jitter.popTo(nullptr, 0, info, 1);
                _lateDrops++;
                continue;
            }

            if (!_jitter.popTo(_playBuffer,
                               kPlayBufferSamples, info,
                               1)) {
                // Do not spin if the frame cannot be popped; the buffer
                // may be in an inconsistent state for a moment.
                vTaskDelay(1);
                break;
            }

            if (!emitFrame(info)) {
                // The ring is full: pushing the rest of the backlog would
                // only block again and widen the stall.
                break;
            }
        }

        // Keep a cushion queued in the I2S ring. The drain above only
        // releases frames whose timestamp is already due, so after a late
        // wakeup the ring runs dry and the I2S task has to insert silence -
        // that silence between frames is the crackle. Moving extra frames
        // into the ring costs nothing but latency: they still play in order,
        // paced by the I2S clock itself.
        if (_i2s != nullptr) {
            constexpr size_t kI2sCushionBytes = 64 * 1024;
            size_t buffered = _i2s->bufferedBytes();
            for (int i = 0; i < 16 && buffered < kI2sCushionBytes; ++i) {
                if (!_running.load() || _paused.load()) {
                    break;
                }
                if (!_jitter.peekOldest(info)) {
                    break;
                }
                // Never queue a frame that is already too old to play.
                if (esp_timer_get_time() - frameDueUs(info.timestamp) >
                    static_cast<int64_t>(kMaxLateFrameMs) * 1000LL) {
                    break;
                }
                if (!_jitter.popTo(_playBuffer, kPlayBufferSamples, info, 1)) {
                    break;
                }
                if (!emitFrame(info)) {
                    break;
                }
                buffered = _i2s->bufferedBytes();
            }
        }
    }
}

void AudioPipeline::timingLoop()
{
    ESP_LOGI(TAG, "Timing client started");
    int64_t lastRequestUs = 0;
    uint32_t requestCount = 0;
    uint8_t packet[64];

    while (_running.load()) {
        int64_t nowUs = esp_timer_get_time();
        int64_t intervalUs = (requestCount < 3) ? 300000 : 3000000;
        if (lastRequestUs == 0 || (nowUs - lastRequestUs) >= intervalUs) {
            sendTimingRequest();
            lastRequestUs = nowUs;
            requestCount++;
        }

        struct sockaddr_in from = {};
        socklen_t fromLen = sizeof(from);
        int received = recvfrom(_timingFd, packet, sizeof(packet), 0,
                                reinterpret_cast<struct sockaddr*>(&from),
                                &fromLen);
        if (received < 0) {
            continue;
        }

        if (received == 32 && packet[1] == 0xD3) {
            _timingReplies++;
            if (_timingReplies <= 3) {
                ESP_LOGD(TAG, "Timing reply received (%u)",
                         static_cast<unsigned>(_timingReplies));
            }
            continue;
        }

        if (received < 48) {
            continue;
        }

        uint8_t response[48] = {};
        response[0] = 0x24; // LI=0, VN=4, mode=4 (server)
        memcpy(response + 24, packet + 40, 8); // origin = client's transmit
        int64_t responseUs = esp_timer_get_time();
        writeNtpTimestamp(response + 32, responseUs);
        writeNtpTimestamp(response + 40, responseUs);

        sendto(_timingFd, response, sizeof(response), 0,
               reinterpret_cast<struct sockaddr*>(&from), fromLen);
    }
}

} // namespace airplay
