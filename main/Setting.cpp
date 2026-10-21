#include "Setting.h"
#include "NvsHelper.h"
#include <atomic>
#include <cstring>

esp_err_t Setting::setAudioSource(AudioSource source) {

    NvsHelper::save(SETTING_NVS_NAMESPACE, SETTING_NVS_KEY_AUDIO_SOURCE,
                    reinterpret_cast<const uint8_t *>(&source), sizeof(AudioSource));

    return ESP_OK;
}

AudioSource Setting::getAudioSource() {
    // Default to the Bluetooth path so a missing or unreadable NVS key
    // cannot leave the enum holding uninitialised stack garbage.
    AudioSource source = AudioSource::A2DP;
    NvsHelper::load(SETTING_NVS_NAMESPACE, SETTING_NVS_KEY_AUDIO_SOURCE,
                    reinterpret_cast<uint8_t *>(&source), sizeof(AudioSource));
    if (source != AudioSource::A2DP && source != AudioSource::AIRPLAY) {
        source = AudioSource::A2DP;
    }
    return source;
}

esp_err_t Setting::setWifiCredentials(const char* ssid, const char* password) {
    if (!ssid || strlen(ssid) == 0 || strlen(ssid) > 32) return ESP_ERR_INVALID_ARG;
    if (password && strlen(password) > 64) return ESP_ERR_INVALID_ARG;

    NvsHelper::save(SETTING_NVS_NAMESPACE, SETTING_NVS_KEY_WIFI_SSID, reinterpret_cast<const uint8_t *>(ssid), strlen(ssid) + 1);
    NvsHelper::save(SETTING_NVS_NAMESPACE, SETTING_NVS_KEY_WIFI_PASS, reinterpret_cast<const uint8_t *>(password), strlen(password) + 1);

    return ESP_OK;
}

esp_err_t Setting::getWifiCredentials(char* ssid, size_t ssid_len, char* password, size_t password_len) {
    if (ssid_len < 33 || password_len < 65) return ESP_ERR_INVALID_ARG;

    std::memset(ssid, 0, ssid_len);
    std::memset(password, 0, password_len);

    esp_err_t ssid_err = NvsHelper::load(SETTING_NVS_NAMESPACE, SETTING_NVS_KEY_WIFI_SSID,
                                     reinterpret_cast<uint8_t *>(ssid), ssid_len);
    esp_err_t pass_err = NvsHelper::load(SETTING_NVS_NAMESPACE, SETTING_NVS_KEY_WIFI_PASS,
                                     reinterpret_cast<uint8_t *>(password), password_len);

    return (ssid_err == ESP_OK && pass_err == ESP_OK) ? ESP_OK : ESP_ERR_NOT_FOUND;

}

esp_err_t Setting::clearStaCredentials()
{
    NvsHelper::save(SETTING_NVS_NAMESPACE, SETTING_NVS_KEY_WIFI_SSID, (const uint8_t*)"", 1);
    NvsHelper::save(SETTING_NVS_NAMESPACE, SETTING_NVS_KEY_WIFI_PASS, (const uint8_t*)"", 1);
    return ESP_OK;
}

esp_err_t Setting::setShowAlbumArt(bool enabled)
{
    // Keep the cached value authoritative even if NVS fails, so the UI and
    // the AirPlay/Bluetooth paths all see the same state until reboot.
    _show_album_art.store(enabled);
    _show_album_art_loaded.store(true);

    const uint8_t value = enabled ? 1 : 0;
    return NvsHelper::save(SETTING_NVS_NAMESPACE, SETTING_NVS_KEY_SHOW_ALBUM_ART,
                           &value, sizeof(value));
}

bool Setting::getShowAlbumArt()
{
    // Read once: this is queried on every RTSP body and from the AirPlay
    // monitor loop, and NVS access is far too slow for that.
    if (!_show_album_art_loaded.load()) {
        uint8_t value = 1;
        if (NvsHelper::load(SETTING_NVS_NAMESPACE, SETTING_NVS_KEY_SHOW_ALBUM_ART,
                            &value, sizeof(value)) == ESP_OK) {
            _show_album_art.store(value != 0);
        }
        _show_album_art_loaded.store(true);
    }
    return _show_album_art.load();
}

esp_err_t Setting::setA2dpVolume(uint8_t volume)
{
    constexpr uint8_t kMaxAvrcVolume = 127;
    if (volume > kMaxAvrcVolume) {
        return ESP_ERR_INVALID_ARG;
    }
    // Skip the NVS write when the value did not change: phones send an
    // absolute-volume command for every slider step.
    if (_a2dp_volume.load() == static_cast<int>(volume)) {
        return ESP_OK;
    }
    _a2dp_volume.store(volume);
    return NvsHelper::save(SETTING_NVS_NAMESPACE, SETTING_NVS_KEY_A2DP_VOLUME,
                           &volume, sizeof(volume));
}

uint8_t Setting::getA2dpVolume()
{
    constexpr uint8_t kDefaultVolume = 90; // ~-6 dB headroom on the gain curve
    int value = _a2dp_volume.load();
    if (value < 0) {
        uint8_t stored = kDefaultVolume;
        if (NvsHelper::load(SETTING_NVS_NAMESPACE, SETTING_NVS_KEY_A2DP_VOLUME,
                            &stored, sizeof(stored)) != ESP_OK ||
            stored > 127) {
            stored = kDefaultVolume;
        }
        value = stored;
        _a2dp_volume.store(value);
    }
    return static_cast<uint8_t>(value);
}

esp_err_t Setting::setAirplayVolumeDb(float db)
{
    if (db > 0.0f) {
        db = 0.0f;
    }
    if (db < -144.0f) {
        db = -144.0f;
    }
    _airplay_volume_loaded.store(true);
    if (_airplay_volume_db.load() == db) {
        return ESP_OK;
    }
    _airplay_volume_db.store(db);
    return NvsHelper::save(SETTING_NVS_NAMESPACE,
                           SETTING_NVS_KEY_AIRPLAY_VOLUME_DB,
                           reinterpret_cast<const uint8_t*>(&db), sizeof(db));
}

float Setting::getAirplayVolumeDb()
{
    if (!_airplay_volume_loaded.load()) {
        float stored = 0.0f;
        if (NvsHelper::load(SETTING_NVS_NAMESPACE,
                            SETTING_NVS_KEY_AIRPLAY_VOLUME_DB,
                            reinterpret_cast<uint8_t*>(&stored),
                            sizeof(stored)) == ESP_OK &&
            stored <= 0.0f && stored >= -144.0f) {
            _airplay_volume_db.store(stored);
        }
        _airplay_volume_loaded.store(true);
    }
    return _airplay_volume_db.load();
}
