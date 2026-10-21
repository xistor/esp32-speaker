#ifndef __AIR_PLAYER_H__
#define __AIR_PLAYER_H__

#include <memory>

#include "esp_err.h"

#include "AirPlayTypes.h"

class AudioI2s;

namespace airplay {
class AirPlayReceiver;
}

class AirPlayer
{
public:
    AirPlayer();
    ~AirPlayer();

    AirPlayer(const AirPlayer&) = delete;
    AirPlayer& operator=(const AirPlayer&) = delete;

    // Initializes the receiver and the ALAC/crypto subsystems. The I2S
    // output is owned by SpeakerApp and must already be started.
    esp_err_t init(AudioI2s* audioI2s = nullptr);

    // Waits for the STA interface to obtain an IP, then advertises the
    // RAOP service and starts the RTSP server.
    esp_err_t start();

    // Kept for compatibility with the previous mDNS-only implementation.
    esp_err_t bonjour() { return start(); }

    void stop();
    esp_err_t deinit();

    // Forwards an on-device transport button press to the AirPlay sender
    // via DACP. Returns false when no AirPlay session is active.
    bool sendRemoteCommand(airplay::RemoteCommand command);

    // Local play/pause fallback for senders that do not expose DACP.
    void setLocalMuted(bool muted);

    bool isRunning() const;

    // True while an AirPlay sender is connected.
    bool isConnected();

private:
    constexpr static const char* _TAG = "AirPlayer";

    std::unique_ptr<airplay::AirPlayReceiver> _receiver;
};

#endif // __AIR_PLAYER_H__
