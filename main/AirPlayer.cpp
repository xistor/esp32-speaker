#include "AirPlayer.h"

#include "AirPlayReceiver.h"
#include "esp_log.h"

AirPlayer::AirPlayer()
{
}

AirPlayer::~AirPlayer()
{
    deinit();
}

esp_err_t AirPlayer::init(AudioI2s* audioI2s)
{
    if (_receiver != nullptr) {
        return ESP_OK;
    }

    try {
        _receiver = std::make_unique<airplay::AirPlayReceiver>();
    } catch (...) {
        return ESP_ERR_NO_MEM;
    }
    if (_receiver == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = _receiver->init(audioI2s);
    if (err != ESP_OK) {
        ESP_LOGE(_TAG, "AirPlay receiver init failed: %s",
                 esp_err_to_name(err));
        _receiver.reset();
        return err;
    }

    ESP_LOGI(_TAG, "AirPlay receiver initialized");
    return ESP_OK;
}

esp_err_t AirPlayer::start()
{
    if (_receiver == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return _receiver->start();
}

void AirPlayer::stop()
{
    if (_receiver != nullptr) {
        _receiver->stop();
    }
}

esp_err_t AirPlayer::deinit()
{
    if (_receiver != nullptr) {
        _receiver->stop();
        _receiver.reset();
    }
    return ESP_OK;
}

bool AirPlayer::sendRemoteCommand(airplay::RemoteCommand command)
{
    return _receiver != nullptr && _receiver->sendRemoteCommand(command);
}

void AirPlayer::setLocalMuted(bool muted)
{
    if (_receiver != nullptr) {
        _receiver->setLocalMuted(muted);
    }
}

bool AirPlayer::isRunning() const
{
    return _receiver != nullptr && _receiver->isRunning();
}

bool AirPlayer::isConnected()
{
    return _receiver != nullptr && _receiver->isConnected();
}
