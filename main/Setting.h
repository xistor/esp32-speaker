#ifndef __SETTING_H__
#define __SETTING_H__

#include "esp_err.h"

inline constexpr const char* SETTING_NVS_NAMESPACE = "setting";

inline constexpr const char* SETTING_NVS_KEY_AUDIO_SOURCE = "audio_source";
inline constexpr const char* SETTING_NVS_KEY_WIFI_SSID = "wifi_ssid";
inline constexpr const char* SETTING_NVS_KEY_WIFI_PASS = "wifi_pass";

enum class AudioSource {
    NONE = 0,
    A2DP,
    AIRPLAY,
};

class Setting {
public:

    static Setting& instance() {
        static Setting _instance;
        return _instance;
    }

    esp_err_t setAudioSource(AudioSource source);
    AudioSource getAudioSource();

    esp_err_t setWifiCredentials(const char* ssid, const char* password);
    esp_err_t getWifiCredentials(char* ssid, size_t ssid_len, char* password, size_t password_len);
    esp_err_t clearStaCredentials();

private:
    Setting() = default;
    ~Setting() = default;

    int _volume = 50; // Default volume level
    bool _mute = false; // Default mute state
};

#endif // __SETTING_H__