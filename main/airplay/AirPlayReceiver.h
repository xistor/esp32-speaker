#ifndef __AIRPLAY_RECEIVER_H__
#define __AIRPLAY_RECEIVER_H__

#include <atomic>
#include <memory>
#include <thread>

#include "esp_err.h"

#include "AirPlayTypes.h"

class AudioI2s;

namespace airplay {

class AudioPipeline;
class RtspServer;

class AirPlayReceiver {
public:
    AirPlayReceiver() = default;
    ~AirPlayReceiver();

    AirPlayReceiver(const AirPlayReceiver&) = delete;
    AirPlayReceiver& operator=(const AirPlayReceiver&) = delete;

    esp_err_t init(AudioI2s* audioI2s);
    esp_err_t start();
    void stop();
    bool isRunning() const { return _running.load(); }

    // True while an AirPlay sender holds a session (connected).
    bool isConnected();

    bool sendRemoteCommand(RemoteCommand command);
    void setLocalMuted(bool muted);

private:
    void monitorLoop();
    bool startServices();
    void stopServices();

    AudioPipeline* _audio = nullptr;
    RtspServer* _server = nullptr;
    std::atomic<bool> _running{false};
    std::atomic<bool> _servicesStarted{false};
    int64_t _lastRssiLogUs = 0;
    uint32_t _lastIp = 0;
    // Last "md" capability advertised over mDNS, tracked so the runtime
    // album-art setting can be applied without restarting the service.
    bool _lastArtworkEnabled = false;
    std::thread _monitorThread;
};

} // namespace airplay

#endif // __AIRPLAY_RECEIVER_H__
