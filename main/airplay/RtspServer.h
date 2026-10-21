#ifndef __AIRPLAY_RTSP_SERVER_H__
#define __AIRPLAY_RTSP_SERVER_H__

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "AirPlayTypes.h"
#include "DacpClient.h"
#include "DmapParser.h"

class UiMusicPlayer;

namespace airplay {

class AudioPipeline;

struct RtspHeader {
    std::string name;  // lower-case
    std::string value;
};

struct RtspRequest {
    std::string method;
    std::string uri;
    std::string version;
    std::vector<RtspHeader> headers;
    std::string body;

    std::string header(const char* name) const;
    int cseq() const;
};

class RtspServer {
public:
    RtspServer() = default;
    ~RtspServer();

    RtspServer(const RtspServer&) = delete;
    RtspServer& operator=(const RtspServer&) = delete;

    bool start(AudioPipeline* audio, UiMusicPlayer* ui);
    void stop();
    bool isRecording();
    // True while a sender has an RTSP session (ANNOUNCE/SETUP succeeded and
    // no TEARDOWN yet), which is what the status bar shows as "connected".
    bool hasSession();
    bool isRunning() const { return _running.load(); }

    // Queues a DACP command for the current sender. Returns false when
    // there is no active AirPlay session to control.
    bool sendRemoteCommand(RemoteCommand command);

    // Local mute used as a play/pause fallback when the sender does not
    // expose a usable DACP endpoint.
    void setLocalMuted(bool muted);

    // Forgets the artwork de-duplication state so the next artwork push is
    // accepted again (used when Album Art is turned back on mid-track).
    void resetArtworkCache();

private:
    struct Session {
        SdpInfo sdp;
        bool announced = false;
        bool setup = false;
        bool recording = false;
        uint16_t clientControlPort = 0;
        uint16_t clientTimingPort = 0;
        uint16_t serverAudioPort = 0;
        uint16_t serverControlPort = 0;
        uint16_t serverTimingPort = 0;
        uint32_t clientIp = 0;
        int ownerFd = -1;
        std::string dacpId;
        std::string activeRemote;
        DmapMetadata lastMetadata;
        bool lastMetadataValid = false;
        int64_t lastProgressUiUs = 0;
        int lastDurationMs = -1;
    };

    void acceptLoop();
    void clientLoop(int fd);
    bool readRequest(int fd, std::string& buffer, RtspRequest& request);
    bool sendResponse(int fd, const RtspRequest& request, int code,
                      const char* reason, const std::string& extraHeaders,
                      const std::string& body);

    void dispatch(int fd, const RtspRequest& request);
    void handleOptions(int fd, const RtspRequest& request);
    void handleAnnounce(int fd, const RtspRequest& request);
    void handleSetup(int fd, const RtspRequest& request);
    void handleRecord(int fd, const RtspRequest& request);
    void handleSetParameter(int fd, const RtspRequest& request);
    void handleGetParameter(int fd, const RtspRequest& request);
    void handleFlush(int fd, const RtspRequest& request);
    void handlePause(int fd, const RtspRequest& request);
    void handleTeardown(int fd, const RtspRequest& request);
    void handleGet(int fd, const RtspRequest& request);

    void resetSession();
    bool parseTransport(const std::string& value, uint16_t& controlPort,
                        uint16_t& timingPort);
    void handleProgress(const std::string& value);
    void handleDmap(const uint8_t* data, size_t len);
    void handleArtwork(const uint8_t* data, size_t len, uint32_t rtpTime,
                       bool hasRtpTime);
    static void artworkTaskEntry(void* arg);
    bool ensureWorkerTask();
    void postMetadata(const std::string& title, const std::string& artist,
                      int durationMs);
    void postProgress(uint32_t positionMs, uint32_t durationMs,
                      bool updateDuration);
    void applyUiUpdates();
    void decodeArtwork();
    void setPlaying(bool playing);
    void setStopped();

    uint32_t localIpv4() const;
    void localMac(uint8_t mac[6]) const;

    AudioPipeline* _audio = nullptr;
    UiMusicPlayer* _ui = nullptr;

    int _listenFd = -1;
    std::atomic<bool> _running{false};
    std::thread _acceptThread;

    std::mutex _sessionMutex;
    std::shared_ptr<Session> _session;

    std::mutex _clientsMutex;
    // One FreeRTOS task per RTSP connection: created under memory pressure,
    // so std::thread (which throws on failure) is not usable here.
    struct ClientTaskArg {
        RtspServer* server = nullptr;
        int fd = -1;
    };
    static void clientTaskEntry(void* arg);
    // Live RTSP connection tasks. Tasks remove themselves on exit, so this
    // doubles as the concurrency cap and the shutdown wait condition.
    size_t _activeClients = 0;
    std::vector<int> _clientFds;

    uint16_t* _coverPixels = nullptr;
    uint16_t* _coverDecoded = nullptr;
    uint8_t* _artworkBuffer = nullptr;
    size_t _artworkLength = 0;
    uint32_t _pendingArtworkHash = 0;
    // De-duplication state, touched by client threads and cleared by the
    // AirPlay monitor loop when Album Art is re-enabled.
    std::atomic<uint32_t> _lastArtworkHash{0};
    std::atomic<bool> _lastArtworkValid{false};
    std::atomic<uint32_t> _lastArtworkRtpTime{0};
    std::atomic<bool> _lastArtworkRtpTimeValid{false};
    std::atomic<bool> _artworkBusy{false};
    std::atomic<TaskHandle_t> _artworkTask{nullptr};
    std::mutex _artworkMutex;
    bool _artworkReady = false;

    // UI updates are handed to the low-priority worker task: taking the
    // LVGL lock from the RTSP thread can block it for hundreds of ms and
    // delay control handling right when a track change needs it.
    std::mutex _uiMutex;
    std::string _pendingTitle;
    std::string _pendingArtist;
    bool _pendingTitleValid = false;
    bool _pendingArtistValid = false;
    int _pendingDurationMs = -1;
    bool _pendingProgressValid = false;
    uint32_t _pendingPositionMs = 0;
    int _pendingPlaying = -1;
    std::atomic<float> _lastVolumeDb{0.0f};
    std::atomic<bool> _localMuted{false};
    std::atomic<float> _preMuteDb{0.0f};
    // Last time the volume was written to NVS (coalesces slider drags).
    int64_t _lastVolumeSaveUs = 0;

    DacpClient _dacp;
};

} // namespace airplay

#endif // __AIRPLAY_RTSP_SERVER_H__
