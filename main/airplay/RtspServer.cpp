#include "RtspServer.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include "AirPlayCrypto.h"
#include "AirPlayMdns.h"
#include "AudioPipeline.h"
#include "DmapParser.h"
#include "SdpParser.h"
#include "Setting.h"
#include "UiMusicPlayer.h"
#include "WifiMgr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_pthread.h"
#include "jpeg_decoder.h"

namespace airplay {

namespace {

constexpr const char* TAG = "AirPlayRtsp";
constexpr const char* kPublicMethods =
    "ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, "
    "GET_PARAMETER, SET_PARAMETER, POST, GET";
// Each RTSP connection owns a task; cap them so a misbehaving sender cannot
// exhaust the internal heap.
constexpr size_t kMaxClientTasks = 6;

// Cover art needs both the build-time capability and the runtime setting, so
// no image bytes are read, copied or decoded while the user has it switched
// off.
bool artworkEnabled()
{
#ifdef CONFIG_AIRPLAY_ENABLE_ARTWORK
    return Setting::instance().getShowAlbumArt();
#else
    return false;
#endif
}

// Coalesce AirPlay volume updates into at most one NVS write per interval;
// senders can emit a volume command per slider step. The final value is
// flushed when the session ends.
constexpr int64_t kVolumeSaveIntervalUs = 2000000; // 2 s

std::string toLower(const std::string& text)
{
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

std::string trim(const std::string& text)
{
    size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// RAOP senders use RTP-Info: rtptime=<n> as a per-item identifier. Metadata
// and artwork for the same track carry the same value, so it can be used to
// discard repeated artwork before hashing/copying the image.
bool parseRtpTime(const std::string& rtpInfo, uint32_t& rtpTime)
{
    constexpr const char* kKey = "rtptime=";
    size_t position = rtpInfo.find(kKey);
    if (position == std::string::npos) {
        return false;
    }
    position += strlen(kKey);
    char* end = nullptr;
    unsigned long value = std::strtoul(rtpInfo.c_str() + position, &end, 10);
    if (end == rtpInfo.c_str() + position) {
        return false;
    }
    rtpTime = static_cast<uint32_t>(value);
    return true;
}

void configureThread(const char* name, int priority, int stackSize, int core)
{
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.thread_name = name;
    cfg.prio = priority;
    cfg.stack_size = stackSize;
    cfg.pin_to_core = core;
    cfg.inherit_cfg = false;
    esp_pthread_set_cfg(&cfg);
}

bool sendAll(int fd, const void* data, size_t len)
{
    const uint8_t* pointer = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    int64_t deadlineUs = esp_timer_get_time() + 3000000;
    while (sent < len) {
        int result = send(fd, pointer + sent, len - sent, 0);
        if (result > 0) {
            sent += static_cast<size_t>(result);
        } else if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                                  errno == EINTR)) {
            if (esp_timer_get_time() > deadlineUs) {
                return false;
            }
            vTaskDelay(1);
            continue;
        } else {
            return false;
        }
    }
    return true;
}

void resizeRgb565Nearest(const uint16_t* src, uint16_t srcWidth,
                         uint16_t srcHeight, uint16_t* dst,
                         uint16_t dstWidth, uint16_t dstHeight)
{
    if (src == nullptr || dst == nullptr || srcWidth == 0 ||
        srcHeight == 0 || dstWidth == 0 || dstHeight == 0) {
        return;
    }
    for (uint16_t y = 0; y < dstHeight; ++y) {
        uint32_t sourceY = static_cast<uint32_t>(y) * srcHeight / dstHeight;
        const uint16_t* sourceRow = src + sourceY * srcWidth;
        uint16_t* destRow = dst + y * dstWidth;
        for (uint16_t x = 0; x < dstWidth; ++x) {
            uint32_t sourceX =
                static_cast<uint32_t>(x) * srcWidth / dstWidth;
            destRow[x] = sourceRow[sourceX];
        }
    }
}

} // namespace

std::string RtspRequest::header(const char* name) const
{
    std::string wanted = toLower(name);
    for (const auto& item : headers) {
        if (item.name == wanted) {
            return item.value;
        }
    }
    return {};
}

int RtspRequest::cseq() const
{
    std::string value = header("cseq");
    if (value.empty()) {
        return 1;
    }
    return std::atoi(value.c_str());
}

RtspServer::~RtspServer()
{
    stop();
    while (_artworkBusy.load()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (_coverPixels != nullptr) {
        heap_caps_free(_coverPixels);
        _coverPixels = nullptr;
    }
    if (_coverDecoded != nullptr) {
        heap_caps_free(_coverDecoded);
        _coverDecoded = nullptr;
    }
    if (_artworkBuffer != nullptr) {
        heap_caps_free(_artworkBuffer);
        _artworkBuffer = nullptr;
    }
}

bool RtspServer::start(AudioPipeline* audio, UiMusicPlayer* ui)
{
    if (_running.load()) {
        return true;
    }
    if (audio == nullptr) {
        return false;
    }

    _audio = audio;
    _ui = ui;

    // Restore the level the sender last used so a reboot or a new session
    // does not jump to a different loudness before the sender sends its own.
    const float savedDb = Setting::instance().getAirplayVolumeDb();
    _lastVolumeDb.store(savedDb);
    _preMuteDb.store(savedDb);
    _audio->setVolumeDb(savedDb);

    _listenFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (_listenFd < 0) {
        ESP_LOGE(TAG, "socket(TCP) failed: errno=%d", errno);
        return false;
    }

    int reuse = 1;
    setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(kRtspPort);

    if (bind(_listenFd, reinterpret_cast<struct sockaddr*>(&address),
             sizeof(address)) != 0) {
        ESP_LOGE(TAG, "bind(%u) failed: errno=%d", kRtspPort, errno);
        close(_listenFd);
        _listenFd = -1;
        return false;
    }

    if (listen(_listenFd, 4) != 0) {
        ESP_LOGE(TAG, "listen failed: errno=%d", errno);
        close(_listenFd);
        _listenFd = -1;
        return false;
    }

    _running.store(true);
    // Create the UI worker before the audio pipeline and sockets have
    // fragmented the internal heap.
    ensureWorkerTask();
    ESP_LOGI(TAG, "heap before RTSP threads: internal=%u largest=%u psram=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    configureThread("airplay_rtsp", 5, 4096, 0);
    _acceptThread = std::thread(&RtspServer::acceptLoop, this);

    if (!_dacp.start()) {
        ESP_LOGW(TAG, "DACP client start failed");
    }

    ESP_LOGI(TAG, "RTSP server listening on port %u", kRtspPort);
    return true;
}

bool RtspServer::sendRemoteCommand(RemoteCommand command)
{
    return _dacp.send(command);
}

void RtspServer::setLocalMuted(bool muted)
{
    if (muted) {
        if (_localMuted.exchange(true)) {
            return;
        }
        _preMuteDb.store(_lastVolumeDb.load());
        if (_audio != nullptr) {
            _audio->setVolumeDb(-144.0f);
        }
        ESP_LOGI(TAG, "AirPlay local mute on");
    } else {
        if (!_localMuted.exchange(false)) {
            return;
        }
        if (_audio != nullptr) {
            _audio->setVolumeDb(_preMuteDb.load());
        }
        ESP_LOGI(TAG, "AirPlay local mute off");
    }
}

bool RtspServer::isRecording()
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    return _session != nullptr && _session->recording;
}

void RtspServer::resetArtworkCache()
{
    _lastArtworkValid.store(false);
    _lastArtworkRtpTimeValid.store(false);
}

bool RtspServer::hasSession()
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    return _session != nullptr;
}

void RtspServer::stop()
{
    bool wasRunning = _running.exchange(false);
    if (_listenFd >= 0) {
        shutdown(_listenFd, SHUT_RDWR);
        close(_listenFd);
        _listenFd = -1;
    }

    if (_acceptThread.joinable()) {
        _acceptThread.join();
    }

    // The artwork worker only exits once it is deleted from another task.
    // Do this after the client tasks are joined so no client can notify
    // a stale handle.
    {
        std::lock_guard<std::mutex> lock(_clientsMutex);
        for (int fd : _clientFds) {
            shutdown(fd, SHUT_RDWR);
        }
        _clientFds.clear();
    }

    // Wait for the client tasks to observe the shutdown and return.
    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard<std::mutex> lock(_clientsMutex);
            if (_activeClients == 0) {
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Let a cover-art decode finish before the worker is destroyed.
    for (int i = 0; i < 200 && _artworkBusy.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    TaskHandle_t artworkTask = _artworkTask.exchange(nullptr);
    if (artworkTask != nullptr) {
        vTaskDelete(artworkTask);
    }

    _dacp.stop();
    if (_audio != nullptr) {
        _audio->stop();
    }
    resetSession();
    if (_ui != nullptr) {
        // The worker task is already gone, so update the UI directly.
        _ui->setPlaying(false);
    }

    if (wasRunning) {
        ESP_LOGI(TAG, "RTSP server stopped");
    }
}

void RtspServer::acceptLoop()
{
    while (_running.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(_listenFd, &readSet);
        struct timeval timeout = {};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;

        int ready = select(_listenFd + 1, &readSet, nullptr, nullptr,
                           &timeout);
        if (ready <= 0) {
            continue;
        }

        struct sockaddr_in peer = {};
        socklen_t peerLen = sizeof(peer);
        int fd = accept(_listenFd, reinterpret_cast<struct sockaddr*>(&peer),
                        &peerLen);
        if (fd < 0) {
            if (_running.load()) {
                ESP_LOGW(TAG, "accept failed: errno=%d", errno);
            }
            continue;
        }

        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        // Keep the RTSP client socket non-blocking so a stalled TCP peer can
        // never block the RTSP thread inside send()/recv().
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }

        struct timeval sendTimeout = {};
        sendTimeout.tv_sec = 2;
        sendTimeout.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout,
                   sizeof(sendTimeout));

        struct timeval recvTimeout = {};
        recvTimeout.tv_sec = 0;
        recvTimeout.tv_usec = 500000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &recvTimeout,
                   sizeof(recvTimeout));

        ESP_LOGI(TAG, "Client connected from %s:%u",
                 inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));

        {
            std::lock_guard<std::mutex> lock(_clientsMutex);
            if (_activeClients >= kMaxClientTasks) {
                ESP_LOGW(TAG, "Too many RTSP clients (%u); rejecting",
                         static_cast<unsigned>(_activeClients));
                close(fd);
                continue;
            }
            _clientFds.push_back(fd);
            _activeClients++;
        }

        ClientTaskArg* arg = static_cast<ClientTaskArg*>(
            malloc(sizeof(ClientTaskArg)));
        TaskHandle_t handle = nullptr;
        BaseType_t created = pdFAIL;
        if (arg != nullptr) {
            arg->server = this;
            arg->fd = fd;
            created = xTaskCreatePinnedToCore(&RtspServer::clientTaskEntry,
                                              "airplay_rtspc", 8192, arg, 5,
                                              &handle, 0);
            if (created != pdPASS) {
                free(arg);
            }
        }

        if (created != pdPASS) {
            ESP_LOGE(TAG,
                     "RTSP client task creation failed: internal=%u largest=%u",
                     static_cast<unsigned>(
                         heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(
                         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
            std::lock_guard<std::mutex> lock(_clientsMutex);
            auto it = std::find(_clientFds.begin(), _clientFds.end(), fd);
            if (it != _clientFds.end()) {
                _clientFds.erase(it);
            }
            if (_activeClients > 0) {
                _activeClients--;
            }
            close(fd);
            continue;
        }
    }
}

void RtspServer::clientTaskEntry(void* arg)
{
    auto* params = static_cast<ClientTaskArg*>(arg);
    RtspServer* self = params->server;
    const int fd = params->fd;
    free(params);

    if (self != nullptr) {
        self->clientLoop(fd);
        std::lock_guard<std::mutex> lock(self->_clientsMutex);
        if (self->_activeClients > 0) {
            self->_activeClients--;
        }
    }
    vTaskDelete(nullptr);
}

void RtspServer::clientLoop(int fd)
{
    std::string buffer;
    buffer.reserve(4096);

    while (_running.load()) {
        RtspRequest request;
        if (!readRequest(fd, buffer, request)) {
            break;
        }
        dispatch(fd, request);
    }

    bool ownsSession = false;
    {
        std::lock_guard<std::mutex> lock(_sessionMutex);
        ownsSession = _session && _session->ownerFd == fd;
    }
    if (ownsSession) {
        // The sender disappeared without TEARDOWN. Tear the stream down so
        // the next connection starts from a clean state.
        if (_audio != nullptr) {
            _audio->stop();
        }
        resetSession();
        mdnsSetSessionActive(false);
        setStopped();
    }

    {
        std::lock_guard<std::mutex> lock(_clientsMutex);
        auto it = std::find(_clientFds.begin(), _clientFds.end(), fd);
        if (it != _clientFds.end()) {
            _clientFds.erase(it);
        }
    }
    close(fd);
    ESP_LOGI(TAG, "Client disconnected");
}

bool RtspServer::readRequest(int fd, std::string& buffer,
                             RtspRequest& request)
{
    size_t headerEnd = std::string::npos;
    int64_t headerDeadlineUs = esp_timer_get_time() + 15000000;

    while (true) {
        headerEnd = buffer.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            break;
        }
        if (buffer.size() > kMaxRtspHeaderSize) {
            ESP_LOGW(TAG, "RTSP header too large");
            return false;
        }

        char temporary[1024];
        int received = recv(fd, temporary, sizeof(temporary), 0);
        if (received > 0) {
            buffer.append(temporary, static_cast<size_t>(received));
        } else if (received == 0) {
            return false;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK ||
                   errno == EINTR) {
            if (!_running.load()) {
                return false;
            }
            if (esp_timer_get_time() > headerDeadlineUs) {
                ESP_LOGW(TAG, "RTSP header timeout");
                return false;
            }
            vTaskDelay(1);
            continue;
        } else {
            return false;
        }
    }

    size_t headerLength = headerEnd + 4;
    std::string headerText = buffer.substr(0, headerEnd);

    size_t lineEnd = headerText.find("\r\n");
    std::string startLine =
        headerText.substr(0, lineEnd == std::string::npos ? headerText.size()
                                                          : lineEnd);
    std::istringstream startStream(startLine);
    startStream >> request.method >> request.uri >> request.version;
    if (request.method.empty()) {
        ESP_LOGW(TAG, "Invalid RTSP start line: %s", startLine.c_str());
        return false;
    }

    request.headers.clear();
    size_t position =
        lineEnd == std::string::npos ? headerText.size() : lineEnd + 2;
    while (position < headerText.size()) {
        size_t end = headerText.find("\r\n", position);
        std::string line =
            headerText.substr(position, end == std::string::npos
                                           ? headerText.size() - position
                                           : end - position);
        position = end == std::string::npos ? headerText.size() : end + 2;
        if (line.empty()) {
            continue;
        }
        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        RtspHeader item;
        item.name = toLower(trim(line.substr(0, colon)));
        item.value = trim(line.substr(colon + 1));
        request.headers.push_back(std::move(item));
    }

    size_t contentLength = 0;
    std::string contentLengthText = request.header("content-length");
    if (!contentLengthText.empty()) {
        contentLength = static_cast<size_t>(
            std::strtoul(contentLengthText.c_str(), nullptr, 10));
    }
    if (contentLength > kMaxRtspBodySize) {
        ESP_LOGW(TAG, "RTSP body too large: %u",
                 static_cast<unsigned>(contentLength));
        return false;
    }

    // Some senders ignore md=0,2 and keep pushing artwork. Reading a 180 KB
    // JPEG into std::string on every lyric update burns CPU in realloc/copy
    // and can trip the task watchdog, so drain image bodies into a small
    // scratch buffer instead. This applies whenever artwork is off for this
    // build or disabled from the settings page.
    if (!artworkEnabled() && request.method == "SET_PARAMETER" &&
        toLower(request.header("content-type")).rfind("image/", 0) == 0) {
        const size_t bufferedBody = buffer.size() - headerLength;
        const size_t consume =
            bufferedBody < contentLength ? bufferedBody : contentLength;
        buffer.erase(0, headerLength + consume);

        size_t remaining = contentLength - consume;
        int64_t discardDeadlineUs = esp_timer_get_time() + 30000000;
        while (remaining > 0) {
            char temporary[1024];
            size_t chunk = remaining < sizeof(temporary)
                               ? remaining
                               : sizeof(temporary);
            int received = recv(fd, temporary, chunk, 0);
            if (received > 0) {
                remaining -= static_cast<size_t>(received);
            } else if (received == 0) {
                return false;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK ||
                       errno == EINTR) {
                if (!_running.load()) {
                    return false;
                }
                if (esp_timer_get_time() > discardDeadlineUs) {
                    ESP_LOGW(TAG, "RTSP artwork body timeout");
                    return false;
                }
                vTaskDelay(1);
                continue;
            } else {
                return false;
            }
        }
        request.body.clear();
        ESP_LOGD(TAG, "Discarded %u bytes of AirPlay artwork",
                 static_cast<unsigned>(contentLength));
        return true;
    }

    // Move any body bytes already received out of the generic header buffer,
    // then read the rest straight into request.body. Growing buffer to the
    // size of a 180 KB artwork body and then copying it again doubles both
    // the allocation peak and the memcpy time.
    const size_t bufferedBody = buffer.size() - headerLength;
    const size_t initialBody =
        bufferedBody < contentLength ? bufferedBody : contentLength;
    request.body.assign(buffer, headerLength, initialBody);
    buffer.erase(0, headerLength + initialBody);
    request.body.reserve(contentLength);

    int64_t bodyDeadlineUs = esp_timer_get_time() + 30000000;
    while (request.body.size() < contentLength) {
        char temporary[1024];
        int received = recv(fd, temporary, sizeof(temporary), 0);
        if (received > 0) {
            request.body.append(temporary, static_cast<size_t>(received));
        } else if (received == 0) {
            return false;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK ||
                   errno == EINTR) {
            if (!_running.load()) {
                return false;
            }
            if (esp_timer_get_time() > bodyDeadlineUs) {
                ESP_LOGW(TAG, "RTSP body timeout");
                return false;
            }
            vTaskDelay(1);
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool RtspServer::sendResponse(int fd, const RtspRequest& request, int code,
                              const char* reason,
                              const std::string& extraHeaders,
                              const std::string& body)
{
    std::ostringstream response;
    response << "RTSP/1.0 " << code << ' ' << reason << "\r\n";
    response << "CSeq: " << request.cseq() << "\r\n";
    response << "Server: AirTunes/377.40.00\r\n";
    if (!extraHeaders.empty()) {
        response << extraHeaders;
        if (extraHeaders.size() < 2 ||
            extraHeaders.compare(extraHeaders.size() - 2, 2, "\r\n") != 0) {
            response << "\r\n";
        }
    }
    if (!body.empty()) {
        response << "Content-Length: " << body.size() << "\r\n";
    }
    response << "\r\n";
    response << body;

    std::string text = response.str();
    if (!sendAll(fd, text.data(), text.size())) {
        ESP_LOGW(TAG, "send response failed");
        return false;
    }
    return true;
}

void RtspServer::dispatch(int fd, const RtspRequest& request)
{
    // iOS normally puts DACP-ID/Active-Remote on ANNOUNCE, but tolerate
    // senders that only include them on a later request.
    std::string dacpId = request.header("dacp-id");
    std::string activeRemote = request.header("active-remote");
    if (!dacpId.empty() || !activeRemote.empty()) {
        std::shared_ptr<Session> session;
        uint32_t clientIp = 0;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(_sessionMutex);
            session = _session;
            if (session) {
                if (!dacpId.empty() && session->dacpId != dacpId) {
                    session->dacpId = dacpId;
                    changed = true;
                }
                if (!activeRemote.empty() &&
                    session->activeRemote != activeRemote) {
                    session->activeRemote = activeRemote;
                    changed = true;
                }
                clientIp = session->clientIp;
            }
        }
        if (changed) {
            _dacp.setSession(session->dacpId, session->activeRemote,
                             clientIp);
        }
    }

    // SET_PARAMETER / GET_PARAMETER keepalives and progress updates can be
    // frequent; keep them out of the default log level so UART traffic does
    // not disturb audio playback.
    if (request.method == "SET_PARAMETER") {
        ESP_LOGD(TAG, "SET_PARAMETER content-type='%s' length=%u",
                 request.header("content-type").c_str(),
                 static_cast<unsigned>(request.body.size()));
    } else {
        ESP_LOGD(TAG, "%s %s", request.method.c_str(), request.uri.c_str());
    }

    if (request.method == "OPTIONS") {
        handleOptions(fd, request);
    } else if (request.method == "ANNOUNCE") {
        handleAnnounce(fd, request);
    } else if (request.method == "SETUP") {
        handleSetup(fd, request);
    } else if (request.method == "RECORD") {
        handleRecord(fd, request);
    } else if (request.method == "SET_PARAMETER") {
        handleSetParameter(fd, request);
    } else if (request.method == "GET_PARAMETER") {
        handleGetParameter(fd, request);
    } else if (request.method == "FLUSH") {
        handleFlush(fd, request);
    } else if (request.method == "PAUSE") {
        handlePause(fd, request);
    } else if (request.method == "TEARDOWN") {
        handleTeardown(fd, request);
    } else if (request.method == "GET") {
        handleGet(fd, request);
    } else {
        sendResponse(fd, request, 501, "Not Implemented", {}, {});
    }
}

void RtspServer::handleOptions(int fd, const RtspRequest& request)
{
    std::string headers =
        std::string("Public: ") + kPublicMethods + "\r\n" +
        "Audio-Jack-Status: connected; type=analog\r\n";

    std::string challenge = request.header("apple-challenge");
    if (!challenge.empty()) {
        std::vector<uint8_t> challengeBytes;
        if (AirPlayCrypto::base64Decode(challenge, challengeBytes)) {
            uint8_t mac[6] = {};
            localMac(mac);
            std::string response = AirPlayCrypto::instance().appleResponse(
                challengeBytes.data(), challengeBytes.size(), localIpv4(),
                mac);
            if (!response.empty()) {
                headers += "Apple-Response: " + response + "\r\n";
            }
        }
    }

    sendResponse(fd, request, 200, "OK", headers, {});
}

void RtspServer::handleAnnounce(int fd, const RtspRequest& request)
{
    if (request.body.empty()) {
        sendResponse(fd, request, 400, "Bad Request", {}, {});
        return;
    }

    SdpInfo sdp;
    std::string error;
    if (!parseSdp(request.body, sdp, error)) {
        ESP_LOGE(TAG, "ANNOUNCE SDP rejected: %s", error.c_str());
        sendResponse(fd, request, 400, "Bad Request", {}, {});
        return;
    }

    ESP_LOGI(TAG, "ANNOUNCE configure begin");
    if (_audio == nullptr || !_audio->configure(sdp)) {
        ESP_LOGE(TAG, "Audio configure failed");
        sendResponse(fd, request, 500, "Internal Server Error", {}, {});
        return;
    }

    uint32_t clientIp = 0;
    struct sockaddr_in peer = {};
    socklen_t peerLen = sizeof(peer);
    if (getpeername(fd, reinterpret_cast<struct sockaddr*>(&peer),
                    &peerLen) == 0) {
        clientIp = peer.sin_addr.s_addr;
    }

    auto session = std::make_shared<Session>();
    session->sdp = sdp;
    session->announced = true;
    session->ownerFd = fd;
    session->clientIp = clientIp;
    session->dacpId = request.header("dacp-id");
    session->activeRemote = request.header("active-remote");

    {
        std::lock_guard<std::mutex> lock(_sessionMutex);
        _session = session;
    }
    _dacp.setSession(session->dacpId, session->activeRemote, clientIp);

    ESP_LOGI(TAG, "ANNOUNCE configure done");
    setStopped();
    ESP_LOGI(TAG, "ANNOUNCE accepted (DACP-ID=%s Active-Remote=%s)",
             session->dacpId.c_str(), session->activeRemote.c_str());
    sendResponse(fd, request, 200, "OK", {}, {});
}

void RtspServer::handleSetup(int fd, const RtspRequest& request)
{
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(_sessionMutex);
        session = _session;
    }

    if (!session || !session->announced) {
        sendResponse(fd, request, 400, "Bad Request", {}, {});
        return;
    }

    uint16_t clientControlPort = 0;
    uint16_t clientTimingPort = 0;
    parseTransport(request.header("transport"), clientControlPort,
                   clientTimingPort);

    uint32_t clientIp = 0;
    struct sockaddr_in peer = {};
    socklen_t peerLen = sizeof(peer);
    if (getpeername(fd, reinterpret_cast<struct sockaddr*>(&peer),
                    &peerLen) == 0) {
        clientIp = peer.sin_addr.s_addr;
    }

    ESP_LOGI(TAG, "SETUP openPorts begin");
    uint16_t audioPort = 0;
    uint16_t controlPort = 0;
    uint16_t timingPort = 0;
    if (_audio == nullptr ||
        !_audio->openPorts(audioPort, controlPort, timingPort)) {
        ESP_LOGE(TAG, "Unable to open AirPlay UDP ports");
        sendResponse(fd, request, 500, "Internal Server Error", {}, {});
        return;
    }
    ESP_LOGI(TAG, "SETUP openPorts done");

    {
        std::lock_guard<std::mutex> lock(_sessionMutex);
        session->clientControlPort = clientControlPort;
        session->clientTimingPort = clientTimingPort;
        session->serverAudioPort = audioPort;
        session->serverControlPort = controlPort;
        session->serverTimingPort = timingPort;
        session->clientIp = clientIp;
        session->setup = true;
        if (session->dacpId.empty()) {
            session->dacpId = request.header("dacp-id");
        }
        if (session->activeRemote.empty()) {
            session->activeRemote = request.header("active-remote");
        }
    }

    if (_audio != nullptr && clientIp != 0) {
        _audio->setControlPeer(clientIp, clientControlPort, clientTimingPort);
    }
    if (clientIp != 0) {
        _dacp.setSession(session->dacpId, session->activeRemote, clientIp);
    }

    char transport[256];
    snprintf(transport, sizeof(transport),
             "Transport: RTP/AVP/UDP;unicast;mode=record;server_port=%u;"
             "control_port=%u;timing_port=%u\r\n",
             audioPort, controlPort, timingPort);

    std::string headers = transport;
    headers += "Session: 1\r\n";

    ESP_LOGI(TAG, "SETUP: client control=%u timing=%u, server audio=%u control=%u timing=%u",
             clientControlPort, clientTimingPort, audioPort, controlPort,
             timingPort);
    sendResponse(fd, request, 200, "OK", headers, {});
}

void RtspServer::handleRecord(int fd, const RtspRequest& request)
{
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(_sessionMutex);
        session = _session;
    }

    if (!session || !session->setup) {
        sendResponse(fd, request, 400, "Bad Request", {}, {});
        return;
    }

    ESP_LOGI(TAG, "RECORD: starting AirPlay audio");
    if (_audio != nullptr) {
        _audio->start();
        ESP_LOGI(TAG, "RECORD: audio start done");
        _audio->resume();
    }
    {
        std::lock_guard<std::mutex> lock(_sessionMutex);
        session->recording = true;
    }
    ESP_LOGI(TAG, "RECORD: mdns begin");
    mdnsSetSessionActive(true);
    ESP_LOGI(TAG, "RECORD: mdns done");
    // DACP discovery is deferred until the first on-device transport button
    // press, so no mDNS queries run during audio start-up or steady playback.
    setPlaying(true);

    std::string headers =
        "Audio-Latency: " + std::to_string(kAirPlayLatencySamples) + "\r\n";
    headers += "Audio-Jack-Status: connected\r\n";
    headers += "Session: 1\r\n";
    sendResponse(fd, request, 200, "OK", headers, {});
}

void RtspServer::handleSetParameter(int fd, const RtspRequest& request)
{
    std::string contentType =
        toLower(request.header("content-type"));
    std::string volume = request.header("volume");
    std::string progress = request.header("progress");
    std::string rate = request.header("rate");

    if (contentType.find("application/x-dmap-tagged") != std::string::npos) {
        handleDmap(reinterpret_cast<const uint8_t*>(request.body.data()),
                   request.body.size());
    } else if (contentType.find("image/jpeg") != std::string::npos ||
               contentType.find("image/png") != std::string::npos) {
#ifdef CONFIG_AIRPLAY_ENABLE_ARTWORK
        if (artworkEnabled()) {
            uint32_t rtpTime = 0;
            const bool hasRtpTime =
                parseRtpTime(request.header("rtp-info"), rtpTime);
            handleArtwork(reinterpret_cast<const uint8_t*>(request.body.data()),
                          request.body.size(), rtpTime, hasRtpTime);
        }
#endif
    } else if (contentType.find("text/parameters") != std::string::npos) {
        std::istringstream stream(request.body);
        std::string line;
        while (std::getline(stream, line)) {
            line = trim(line);
            if (line.rfind("volume:", 0) == 0) {
                volume = trim(line.substr(strlen("volume:")));
            } else if (line.rfind("progress:", 0) == 0) {
                progress = trim(line.substr(strlen("progress:")));
            } else if (line.rfind("rate:", 0) == 0) {
                rate = trim(line.substr(strlen("rate:")));
            }
        }
    }

    if (!volume.empty()) {
        float db = std::strtof(volume.c_str(), nullptr);
        const float previousDb = _lastVolumeDb.exchange(db);
        if (_localMuted.load()) {
            // Keep the play/pause mute; remember the sender's level so
            // unmuting restores it.
            _preMuteDb.store(db);
        } else if (_audio != nullptr) {
            _audio->setVolumeDb(db);
        }
        if (std::fabs(db - previousDb) >= 0.5f) {
            ESP_LOGI(TAG, "Volume: %.2f dB%s", db,
                     _localMuted.load() ? " (locally muted)" : "");
        }
        if (db != previousDb) {
            // Remember the sender's level; coalesced so a slider drag does
            // not cause one NVS commit per step.
            const int64_t nowUs = esp_timer_get_time();
            if (nowUs - _lastVolumeSaveUs >= kVolumeSaveIntervalUs) {
                _lastVolumeSaveUs = nowUs;
                Setting::instance().setAirplayVolumeDb(db);
            }
        }
    }

    if (!progress.empty()) {
        handleProgress(progress);
    }

    if (!rate.empty()) {
        ESP_LOGI(TAG, "Rate: %s", rate.c_str());
        float value = std::strtof(rate.c_str(), nullptr);
        if (value <= 0.0f) {
            if (_audio != nullptr) {
                _audio->pause();
            }
            {
                std::lock_guard<std::mutex> lock(_sessionMutex);
                if (_session) {
                    _session->recording = false;
                }
            }
            setPlaying(false);
        } else {
            if (_audio != nullptr) {
                _audio->resume();
            }
            {
                std::lock_guard<std::mutex> lock(_sessionMutex);
                if (_session) {
                    _session->recording = true;
                }
            }
            setPlaying(true);
        }
    }

    sendResponse(fd, request, 200, "OK", {}, {});
}

void RtspServer::handleGetParameter(int fd, const RtspRequest& request)
{
    char body[64];
    snprintf(body, sizeof(body), "volume: %.6f\r\n", _lastVolumeDb.load());
    sendResponse(fd, request, 200, "OK",
                 "Content-Type: text/parameters\r\n", body);
}

void RtspServer::handleFlush(int fd, const RtspRequest& request)
{
    ESP_LOGI(TAG, "FLUSH begin");
    if (_audio != nullptr) {
        // Keep the I2S tail so the previous track's last few milliseconds
        // bridge the gap while the next track is decoded and re-anchored.
        _audio->flush(false);
    }
    ESP_LOGI(TAG, "FLUSH done");
    sendResponse(fd, request, 200, "OK", {}, {});
}

void RtspServer::handlePause(int fd, const RtspRequest& request)
{
    ESP_LOGI(TAG, "PAUSE begin");
    if (_audio != nullptr) {
        _audio->pause();
    }
    ESP_LOGI(TAG, "PAUSE done");
    {
        std::lock_guard<std::mutex> lock(_sessionMutex);
        if (_session) {
            _session->recording = false;
        }
    }
    setPlaying(false);
    sendResponse(fd, request, 200, "OK", {}, {});
}

void RtspServer::handleTeardown(int fd, const RtspRequest& request)
{
    ESP_LOGI(TAG, "TEARDOWN begin");
    if (_audio != nullptr) {
        _audio->stop();
    }
    ESP_LOGI(TAG, "TEARDOWN done");
    resetSession();
    mdnsSetSessionActive(false);
    setStopped();
    sendResponse(fd, request, 200, "OK", {}, {});
}

void RtspServer::handleGet(int fd, const RtspRequest& request)
{
    // AirPlay 1 does not need /info. AirPlay 2 clients will ask for it; that
    // path will be added together with HAP pairing.
    if (request.uri == "/info") {
        sendResponse(fd, request, 404, "Not Found", {}, {});
    } else {
        sendResponse(fd, request, 404, "Not Found", {}, {});
    }
}

void RtspServer::resetSession()
{
    {
        std::lock_guard<std::mutex> lock(_sessionMutex);
        _session.reset();
        _dacp.clearSession();
        if (_localMuted.exchange(false) && _audio != nullptr) {
            _audio->setVolumeDb(_preMuteDb.load());
        }
        _preMuteDb.store(0.0f);
    }
    // Persist the sender's last level so the next session (or a reboot)
    // starts at the same loudness. Done outside the session lock because an
    // NVS commit takes milliseconds and must not block status queries.
    Setting::instance().setAirplayVolumeDb(_lastVolumeDb.load());
    _lastVolumeSaveUs = esp_timer_get_time();
}

bool RtspServer::parseTransport(const std::string& value,
                                uint16_t& controlPort, uint16_t& timingPort)
{
    controlPort = 0;
    timingPort = 0;
    if (value.empty()) {
        return false;
    }

    const char* control = strstr(value.c_str(), "control_port=");
    if (control != nullptr) {
        controlPort = static_cast<uint16_t>(
            std::strtoul(control + strlen("control_port="), nullptr, 10));
    }
    const char* timing = strstr(value.c_str(), "timing_port=");
    if (timing != nullptr) {
        timingPort = static_cast<uint16_t>(
            std::strtoul(timing + strlen("timing_port="), nullptr, 10));
    }
    return true;
}

void RtspServer::handleProgress(const std::string& value)
{
    if (_ui == nullptr) {
        return;
    }

    uint64_t start = 0;
    uint64_t current = 0;
    uint64_t end = 0;
    const char* cursor = value.c_str();
    char* next = nullptr;
    start = std::strtoull(cursor, &next, 10);
    if (next == nullptr || *next != '/') {
        return;
    }
    current = std::strtoull(next + 1, &next, 10);
    if (next == nullptr || *next != '/') {
        return;
    }
    end = std::strtoull(next + 1, &next, 10);

    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(_sessionMutex);
        session = _session;
    }
    if (!session || session->sdp.sampleRate <= 0) {
        return;
    }

    uint32_t rate = static_cast<uint32_t>(session->sdp.sampleRate);
    uint64_t positionSamples = current > start ? current - start : 0;
    uint64_t durationSamples = end > start ? end - start : 0;
    uint32_t positionMs =
        static_cast<uint32_t>((positionSamples * 1000ULL) / rate);
    uint32_t durationMs =
        static_cast<uint32_t>((durationSamples * 1000ULL) / rate);
    if (durationMs > 0 && positionMs > durationMs) {
        positionMs = durationMs;
    }

    ESP_LOGD(TAG,
             "Progress: %llu/%llu/%llu -> %u/%u ms",
             static_cast<unsigned long long>(start),
             static_cast<unsigned long long>(current),
             static_cast<unsigned long long>(end),
             static_cast<unsigned>(positionMs),
             static_cast<unsigned>(durationMs));

    if (_ui == nullptr) {
        return;
    }

    // The sender emits progress several times per second; redrawing the
    // slider at that rate needlessly competes with audio for LVGL/PSRAM.
    bool updateDuration = false;
    int64_t nowUs = esp_timer_get_time();
    {
        std::lock_guard<std::mutex> lock(_sessionMutex);
        if (durationMs > 0 &&
            static_cast<int>(durationMs) != session->lastDurationMs) {
            session->lastDurationMs = static_cast<int>(durationMs);
            updateDuration = true;
        }
        if (session->lastProgressUiUs != 0 &&
            (nowUs - session->lastProgressUiUs) < 500000) {
            return;
        }
        session->lastProgressUiUs = nowUs;
    }

    if (updateDuration) {
        postProgress(positionMs, durationMs, true);
    } else {
        postProgress(positionMs, durationMs, false);
    }
}

void RtspServer::handleDmap(const uint8_t* data, size_t len)
{
    DmapMetadata metadata;
    if (!parseDmap(data, len, metadata)) {
        return;
    }

    // Senders often repeat an identical DMAP payload several times in a
    // burst. Updating three LVGL labels for every copy causes a visible
    // audio hiccup, so only apply a genuinely new metadata set.
    {
        std::lock_guard<std::mutex> lock(_sessionMutex);
        if (!_session) {
            return;
        }
        Session& session = *_session;
        if (session.lastMetadataValid &&
            session.lastMetadata.title == metadata.title &&
            session.lastMetadata.artist == metadata.artist &&
            session.lastMetadata.album == metadata.album &&
            session.lastMetadata.durationMs == metadata.durationMs) {
            return;
        }
        session.lastMetadata = metadata;
        session.lastMetadataValid = true;
    }

    if (_ui == nullptr) {
        return;
    }
    // Hand the LVGL work to the UI worker. Touching LVGL from the RTSP
    // thread can stall control handling for hundreds of milliseconds.
    postMetadata(metadata.title, metadata.artist, metadata.durationMs);

    ESP_LOGI(TAG, "Metadata: title='%s' artist='%s' album='%s' duration=%d",
             metadata.title.c_str(), metadata.artist.c_str(),
             metadata.album.c_str(), metadata.durationMs);
}

void RtspServer::handleArtwork(const uint8_t* data, size_t len,
                               uint32_t rtpTime, bool hasRtpTime)
{
    if (_ui == nullptr || data == nullptr || len == 0 ||
        len > kMaxRtspBodySize) {
        return;
    }

    // RAOP does not have an artwork handle like AVRCP. The RTP-Info rtptime
    // is the closest equivalent: all metadata/artwork for one track shares
    // it. Senders such as QQ Music resend the same JPEG with every lyric
    // update, so drop a repeated rtptime before hashing or copying.
    if (hasRtpTime && _lastArtworkRtpTimeValid &&
        rtpTime == _lastArtworkRtpTime) {
        ESP_LOGD(TAG, "Cover art already handled for rtptime=%u; ignored",
                 static_cast<unsigned>(rtpTime));
        return;
    }

    // Fallback for senders that omit RTP-Info: hash the buffer before copying
    // it so an unchanged cover costs one pass over the data instead of a
    // 180 KB copy here plus a second pass in the worker.
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    if (_lastArtworkValid && hash == _lastArtworkHash) {
        ESP_LOGD(TAG, "Cover art unchanged (%u bytes); ignored",
                 static_cast<unsigned>(len));
        return;
    }

    if (_artworkBusy.exchange(true)) {
        ESP_LOGD(TAG, "Artwork busy, dropping %u bytes",
                 static_cast<unsigned>(len));
        return;
    }

    if (_artworkBuffer == nullptr) {
        _artworkBuffer = static_cast<uint8_t*>(
            heap_caps_malloc(kMaxRtspBodySize,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (_artworkBuffer == nullptr) {
            ESP_LOGW(TAG, "Artwork buffer allocation failed");
            _artworkBusy.store(false);
            return;
        }
    }

    memcpy(_artworkBuffer, data, len);
    {
        std::lock_guard<std::mutex> lock(_artworkMutex);
        _artworkLength = len;
        _pendingArtworkHash = hash;
        _artworkReady = true;
    }

    if (!ensureWorkerTask()) {
        _artworkBusy.store(false);
        return;
    }
    if (hasRtpTime) {
        _lastArtworkRtpTime = rtpTime;
        _lastArtworkRtpTimeValid = true;
    }
    xTaskNotifyGive(_artworkTask.load());
}

// The UI worker is created once and kept alive. It is best created from
// start(), while internal RAM is still in large blocks: creating it on the
// first artwork used to fail once Wi-Fi and the audio pipeline had
// fragmented the internal heap.
bool RtspServer::ensureWorkerTask()
{
    if (_artworkTask.load() != nullptr) {
        return true;
    }

    // JPEG decoding needs a few KB of stack; fall back to smaller sizes
    // rather than losing cover art entirely.
    static const uint32_t kStackSizes[] = {8192, 6144, 4096};
    for (uint32_t stackSize : kStackSizes) {
        TaskHandle_t handle = nullptr;
        // Core 0 is busy with Wi-Fi, RTSP and the software renderer, which
        // starved JPEG decoding for seconds. Core 1 has the audio workers
        // (priority 8+) above this task, so they still preempt it, but it
        // gets the idle time instead of waiting behind swdraw.
        if (xTaskCreatePinnedToCore(&RtspServer::artworkTaskEntry,
                                    "airplay_ui", stackSize, this, 4,
                                    &handle, 1) == pdPASS) {
            _artworkTask.store(handle);
            return true;
        }
    }

    ESP_LOGW(TAG, "UI worker creation failed");
    return false;
}

void RtspServer::artworkTaskEntry(void* arg)
{
    auto* self = static_cast<RtspServer*>(arg);
    while (true) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)) == 0) {
            continue;
        }
        if (!self->_running.load()) {
            continue;
        }

        self->applyUiUpdates();

        bool artworkReady = false;
        {
            std::lock_guard<std::mutex> lock(self->_artworkMutex);
            artworkReady = self->_artworkReady;
            self->_artworkReady = false;
        }
        if (artworkReady) {
            self->decodeArtwork();
            self->_artworkBusy.store(false);
        }
    }
}

void RtspServer::postMetadata(const std::string& title,
                              const std::string& artist, int durationMs)
{
    if (!_running.load()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(_uiMutex);
        if (!title.empty()) {
            _pendingTitle = title;
            _pendingTitleValid = true;
        }
        if (!artist.empty()) {
            _pendingArtist = artist;
            _pendingArtistValid = true;
        }
        if (durationMs >= 0) {
            _pendingDurationMs = durationMs;
        }
    }

    if (ensureWorkerTask()) {
        xTaskNotifyGive(_artworkTask.load());
    }
}

void RtspServer::postProgress(uint32_t positionMs, uint32_t durationMs,
                              bool updateDuration)
{
    if (!_running.load()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(_uiMutex);
        _pendingPositionMs = positionMs;
        _pendingProgressValid = true;
        if (updateDuration && durationMs > 0) {
            _pendingDurationMs = static_cast<int>(durationMs);
        }
    }

    if (ensureWorkerTask()) {
        xTaskNotifyGive(_artworkTask.load());
    }
}

void RtspServer::applyUiUpdates()
{
    if (_ui == nullptr) {
        return;
    }

    std::string title;
    std::string artist;
    bool titleValid = false;
    bool artistValid = false;
    bool progressValid = false;
    uint32_t positionMs = 0;
    int durationMs = -1;
    int playing = -1;
    {
        std::lock_guard<std::mutex> lock(_uiMutex);
        titleValid = _pendingTitleValid;
        artistValid = _pendingArtistValid;
        progressValid = _pendingProgressValid;
        positionMs = _pendingPositionMs;
        durationMs = _pendingDurationMs;
        playing = _pendingPlaying;
        if (titleValid) {
            title.swap(_pendingTitle);
        }
        if (artistValid) {
            artist.swap(_pendingArtist);
        }
        _pendingTitleValid = false;
        _pendingArtistValid = false;
        _pendingProgressValid = false;
        _pendingDurationMs = -1;
        _pendingPlaying = -1;
    }

    if (playing >= 0) {
        _ui->setPlaying(playing != 0);
    }
    // One lock acquisition for the whole metadata set: taking the LVGL lock
    // four times per lyric line is what made the UI worker wait 500 ms per
    // call while the LVGL task was busy rendering.
    if (titleValid || artistValid || durationMs >= 0) {
        _ui->applyMetadata(titleValid ? title.c_str() : nullptr,
                           artistValid ? artist.c_str() : nullptr,
                           durationMs);
    }
    if (progressValid) {
        _ui->setPlayPosition(positionMs);
    }
}

void RtspServer::decodeArtwork()
{
    if (_ui == nullptr || _artworkBuffer == nullptr || _artworkLength == 0) {
        return;
    }

    // The hash was computed by the producer while the image was still in the
    // RTSP body, so an unchanged cover never reaches this function.
    const uint32_t hash = _pendingArtworkHash;
    if (_lastArtworkValid && hash == _lastArtworkHash) {
        ESP_LOGD(TAG, "Cover art unchanged; skipping decode");
        return;
    }

    // Audio playback has priority over cover art. Wait for a healthy
    // jitter buffer instead of decoding a large JPEG while a track change
    // is still refilling the pipeline.
    if (_audio != nullptr) {
        int64_t deadlineUs = esp_timer_get_time() + 3000000;
        bool ready = false;
        while (_running.load() && esp_timer_get_time() < deadlineUs) {
            // Keep the UI in sync while we wait, so play state and
            // metadata are not held back by the cover-art gate.
            applyUiUpdates();
            if (!_audio->isPlaybackReady()) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            // Require a short stable window: a track change may start just
            // after the first check.
            vTaskDelay(pdMS_TO_TICKS(200));
            ready = _running.load() && _audio->isPlaybackReady();
            if (ready) {
                break;
            }
        }
        if (!ready) {
            ESP_LOGD(TAG, "Cover art decode skipped: audio is busy");
            return;
        }
    }

    // The album-art widget is 130x130. Decoding to 200x200 only made the
    // software renderer scale a larger buffer on every redraw.
    constexpr uint16_t kCoverSize = 130;
    if (_coverPixels == nullptr) {
        _coverPixels = static_cast<uint16_t*>(
            heap_caps_malloc(kCoverSize * kCoverSize * sizeof(uint16_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (_coverPixels == nullptr) {
            ESP_LOGW(TAG, "Cover art buffer allocation failed");
            return;
        }
    }
    if (_coverDecoded == nullptr) {
        _coverDecoded = static_cast<uint16_t*>(
            heap_caps_malloc(400 * 400 * sizeof(uint16_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (_coverDecoded == nullptr) {
            ESP_LOGW(TAG, "Cover art decode buffer allocation failed");
            return;
        }
    }

    esp_jpeg_image_cfg_t config = {};
    config.indata = _artworkBuffer;
    config.indata_size = static_cast<uint32_t>(_artworkLength);
    config.outbuf = reinterpret_cast<uint8_t*>(_coverDecoded);
    config.outbuf_size = 400 * 400 * sizeof(uint16_t);
    config.out_format = JPEG_IMAGE_FORMAT_RGB565;
    config.out_scale = JPEG_IMAGE_SCALE_0;
    config.flags.swap_color_bytes = 1;

    esp_jpeg_image_output_t source = {};
    if (esp_jpeg_get_image_info(&config, &source) == ESP_OK) {
        uint16_t maxDimension =
            source.width > source.height ? source.width : source.height;
        if (maxDimension > 3200) {
            ESP_LOGW(TAG, "Cover art too large: %ux%u", source.width,
                     source.height);
            return;
        }
        if (maxDimension > 1600) {
            config.out_scale = JPEG_IMAGE_SCALE_1_8;
        } else if (maxDimension > 400) {
            // The cover is displayed at 130x130, so decode to ~200 px at
            // most. A 512x512 source used to decode at 256x256 (65k pixels,
            // ~530 ms) while the decoder hammers PSRAM; 128x128 is both
            // sharper on screen and four times cheaper.
            config.out_scale = JPEG_IMAGE_SCALE_1_4;
        } else if (maxDimension > 200) {
            config.out_scale = JPEG_IMAGE_SCALE_1_2;
        }
        ESP_LOGI(TAG, "Cover art source: %ux%u scale=%d",
                 source.width, source.height,
                 static_cast<int>(config.out_scale));
    }

    ESP_LOGI(TAG, "Cover art decode start: %u bytes",
             static_cast<unsigned>(_artworkLength));
    esp_jpeg_image_output_t output = {};
    esp_err_t error = ESP_FAIL;
    int64_t decodeStartUs = esp_timer_get_time();
    const esp_jpeg_image_scale_t initialScale = config.out_scale;
    const esp_jpeg_image_scale_t scales[] = {
        initialScale, JPEG_IMAGE_SCALE_1_2, JPEG_IMAGE_SCALE_1_4,
        JPEG_IMAGE_SCALE_1_8};
    for (esp_jpeg_image_scale_t scale : scales) {
        if (scale < initialScale) {
            continue;
        }
        config.out_scale = scale;
        error = esp_jpeg_decode(&config, &output);
        if (error == ESP_OK && output.width <= 400 && output.height <= 400) {
            break;
        }
        error = ESP_FAIL;
    }
    ESP_LOGI(TAG, "Cover art decode: %u bytes, %lld ms",
             static_cast<unsigned>(_artworkLength),
             static_cast<long long>((esp_timer_get_time() - decodeStartUs) / 1000));
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Cover art JPEG decode failed: %s",
                 esp_err_to_name(error));
        return;
    }

    resizeRgb565Nearest(_coverDecoded, output.width, output.height,
                        _coverPixels, kCoverSize, kCoverSize);
    _ui->setCoverArt(reinterpret_cast<uint8_t*>(_coverPixels), kCoverSize,
                     kCoverSize);
    _lastArtworkHash = hash;
    _lastArtworkValid = true;
    ESP_LOGI(TAG, "Cover art updated: %ux%u -> %ux%u", output.width,
             output.height, kCoverSize, kCoverSize);
}

void RtspServer::setPlaying(bool playing)
{
    if (_ui == nullptr || !_running.load()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(_uiMutex);
        _pendingPlaying = playing ? 1 : 0;
    }
    if (ensureWorkerTask()) {
        xTaskNotifyGive(_artworkTask.load());
    }
}

void RtspServer::setStopped()
{
    setPlaying(false);
}

uint32_t RtspServer::localIpv4() const
{
    auto& wifi = WifiMgr::instance();
    esp_netif_t* netif = wifi.getStaNetif();
    if (netif == nullptr) {
        return 0;
    }
    esp_netif_ip_info_t info = {};
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) {
        return 0;
    }
    return info.ip.addr;
}

void RtspServer::localMac(uint8_t mac[6]) const
{
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
}

} // namespace airplay
