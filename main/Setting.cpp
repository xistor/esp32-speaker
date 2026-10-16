#include "Setting.h"
#include "NvsHelper.h"
#include <cstring>

esp_err_t Setting::setAudioSource(AudioSource source) {

    NvsHelper::save(SETTING_NVS_NAMESPACE, SETTING_NVS_KEY_AUDIO_SOURCE,
                    reinterpret_cast<const uint8_t *>(&source), sizeof(AudioSource));

    return ESP_OK;
}

AudioSource Setting::getAudioSource() {
    AudioSource source;
    NvsHelper::load(SETTING_NVS_NAMESPACE, SETTING_NVS_KEY_AUDIO_SOURCE,
                    reinterpret_cast<uint8_t *>(&source), sizeof(AudioSource));
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
