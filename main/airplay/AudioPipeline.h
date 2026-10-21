#ifndef __AIRPLAY_AUDIO_PIPELINE_H__
#define __AIRPLAY_AUDIO_PIPELINE_H__

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "AirPlayTypes.h"
#include "AlacDecoder.h"
#include "PcmJitterBuffer.h"
#include "psa/crypto.h"

class AudioI2s;
class UiMusicPlayer;

namespace airplay {

class AudioPipeline {
public:
    AudioPipeline() = default;
    ~AudioPipeline();

    AudioPipeline(const AudioPipeline&) = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;

    esp_err_t init(AudioI2s* audioI2s);

    // Reconfigure for a new ANNOUNCE. Stops any running stream first.
    bool configure(const SdpInfo& sdp);

    // Opens the three UDP sockets and returns their local ports. The
    // sockets stay open until stop()/closePorts().
    bool openPorts(uint16_t& audioPort, uint16_t& controlPort,
                   uint16_t& timingPort);

    void start();
    void stop();
    // clearI2s=false keeps the tail of the previous track in the I2S ring,
    // which bridges the short gap while the next track is being rebuffered.
    void flush(bool clearI2s = true);
    void pause();
    void resume();

    void setVolumeDb(float db);
    float volumeDb() const { return _volumeDb.load(); }
    bool isRtpStalled(uint32_t timeoutMs) const;

    // True when decoding another PCM stream (e.g. cover art) cannot disturb
    // playback: the stream is stopped/paused, or it is playing with a
    // healthy jitter buffer.
    bool isPlaybackReady() const;

    // Feeds the UI spectrum analyser with the PCM actually sent to I2S.
    // Decoding the spectrum happens on its own low-priority task.
    void setVisualizer(UiMusicPlayer* ui);

    // Sender control/timing endpoints, taken from the SETUP Transport header.
    void setControlPeer(uint32_t ipv4, uint16_t controlPort, uint16_t timingPort);

    uint16_t audioPort() const { return _audioPort; }
    uint16_t controlPort() const { return _controlPort; }
    uint16_t timingPort() const { return _timingPort; }

private:
    void rtpLoop();
    void playbackLoop();
    void timingLoop();
    void handleRtpPacket(const uint8_t* data, size_t len);
    void closePorts();
    void stopInternal(bool releaseResources);
    // Waits for the audio tasks created by start() to finish (they signal
    // _taskExitSem right before deleting themselves).
    void joinWorkerTasks(uint32_t timeoutMs);
    void requestResend(uint16_t firstSequence, uint16_t count);
    void sendTimingRequest();
    void sendRtcpReceiverReport();
    bool openUdpSocket(int& fd, uint16_t& port);
    void updateAnchor();

    AudioI2s* _i2s = nullptr;
    std::atomic<UiMusicPlayer*> _visualizer{nullptr};
    SdpInfo _sdp;
    AlacDecoder _decoder;
    std::mutex _decoderMutex;
    PcmJitterBuffer _jitter;

    psa_key_id_t _aesKey = 0;
    int _audioFd = -1;
    int _controlFd = -1;
    int _timingFd = -1;
    uint16_t _audioPort = 0;
    uint16_t _controlPort = 0;
    uint16_t _timingPort = 0;

    std::atomic<bool> _running{false};
    std::atomic<bool> _paused{false};
    std::atomic<float> _volumeDb{0.0f};

    std::thread _acceptThread;

    // Audio workers are plain FreeRTOS tasks: std::thread failure throws
    // std::system_error, and exceptions are disabled here, so a memory
    // shortage would abort the device instead of failing gracefully.
    static void rtpTaskEntry(void* arg);
    static void playbackTaskEntry(void* arg);
    static void timingTaskEntry(void* arg);
    TaskHandle_t _rtpTask = nullptr;
    TaskHandle_t _playTask = nullptr;
    TaskHandle_t _timingTask = nullptr;
    SemaphoreHandle_t _taskExitSem = nullptr;

    // Large audio buffers live in PSRAM so the internal heap keeps enough
    // room for the FreeRTOS task stacks and Wi-Fi buffers.
    uint8_t* _rtpBuffer = nullptr;
    uint8_t* _decryptBuffer = nullptr;
    int16_t* _pcmBuffer = nullptr;
    int16_t* _playBuffer = nullptr;

    std::atomic<bool> _needAnchor{true};
    std::atomic<bool> _fastRebuffer{false};
    std::atomic<bool> _deepRebuffer{false};
    bool _inUnderrun = false;
    int64_t _thinStartUs = 0;
    uint32_t _anchorRtp = 0;
    int64_t _anchorLocalUs = 0;
    uint32_t _prebufferFrames = 8;
    std::atomic<uint32_t> _lastPacketTimestamp{0};
    std::atomic<bool> _lastPacketTimestampValid{false};
    std::atomic<int64_t> _underrunStartUs{0};

    uint32_t _datagramCount = 0;
    uint32_t _invalidPackets = 0;
    uint32_t _packetCount = 0;
    uint32_t _frameCount = 0;
    uint32_t _diagPackets = 0;
    std::atomic<uint16_t> _lastSeq{0};
    std::atomic<bool> _seqValid{false};
    uint32_t _seqGaps = 0;
    uint32_t _resendRequests = 0;
    uint32_t _controlPeerIp = 0;
    uint16_t _controlPeerPort = 0;
    uint16_t _timingPeerPort = 0;
    uint32_t _senderSsrc = 0;
    uint32_t _firstSequence = 0;
    uint32_t _highestSequence = 0;
    uint32_t _validPackets = 0;
    uint32_t _rtcpReports = 0;
    bool _highestSequenceValid = false;
    int64_t _lastRtcpUs = 0;
    std::atomic<int64_t> _lastDatagramUs{0};
    uint32_t _timingReplies = 0;
    uint32_t _timingRequests = 0;
    int64_t _lastResendUs = 0;
    bool _firstPacketLogged = false;
    uint32_t _decodeErrors = 0;
    uint32_t _lateDrops = 0;
    uint32_t _underruns = 0;
    int64_t _lastStatsUs = 0;
    int64_t _lastWaitLogUs = 0;
    // Longest gap between playback-loop iterations in the current stats
    // window. A large value means the thread was starved, which is what
    // turns into an I2S burst followed by an overflow.
    int64_t _lastPlayLoopUs = 0;
    int64_t _maxPlayGapUs = 0;
    // Longest single I2S ring-buffer push in the current stats window. This
    // is the only call in the playback loop that can block for tens of ms.
    int64_t _maxSendUs = 0;
    // Longest single RTP packet handling in the current stats window, and
    // the time the RTP thread spent decoding in total.
    int64_t _maxRtpUs = 0;
    int64_t _rtpBusyUs = 0;
    int64_t _lastHeapLogUs = 0;
};

} // namespace airplay

#endif // __AIRPLAY_AUDIO_PIPELINE_H__
