#ifndef __SETTING_H__
#define __SETTING_H__

#include "esp_err.h"

#include <atomic>

inline constexpr const char* SETTING_NVS_NAMESPACE = "setting";

inline constexpr const char* SETTING_NVS_KEY_AUDIO_SOURCE = "audio_source";
inline constexpr const char* SETTING_NVS_KEY_WIFI_SSID = "wifi_ssid";
inline constexpr const char* SETTING_NVS_KEY_WIFI_PASS = "wifi_pass";
inline constexpr const char* SETTING_NVS_KEY_SHOW_ALBUM_ART = "show_album_art";
inline constexpr const char* SETTING_NVS_KEY_A2DP_VOLUME = "a2dp_volume";
inline constexpr const char* SETTING_NVS_KEY_AIRPLAY_VOLUME_DB = "airplay_vol_db";

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

    // Whether cover art is displayed on the player page. Disabling it also
    // stops AirPlay from advertising/decoding artwork.
    esp_err_t setShowAlbumArt(bool enabled);
    bool getShowAlbumArt();

    // Last volume used by each source, persisted so a reboot (or reconnect)
    // keeps the same loudness. A2DP uses the AVRCP 0..127 scale, AirPlay the
    // sender's dB scale (-144..0).
    esp_err_t setA2dpVolume(uint8_t volume);
    uint8_t getA2dpVolume();
    esp_err_t setAirplayVolumeDb(float db);
    float getAirplayVolumeDb();

private:
    Setting() = default;
    ~Setting() = default;

    int _volume = 50; // Default volume level
    bool _mute = false; // Default mute state

    // Cached because cover-art checks run from the RTSP thread and the
    // AirPlay monitor loop, where hitting NVS every time is too expensive.
    std::atomic<bool> _show_album_art{true};
    std::atomic<bool> _show_album_art_loaded{false};

    // Volume caches: written by the Bluetooth/RTSP callbacks and read at
    // startup. -1 / loaded==false means "not read from NVS yet".
    std::atomic<int> _a2dp_volume{-1};
    std::atomic<float> _airplay_volume_db{0.0f};
    std::atomic<bool> _airplay_volume_loaded{false};
};

#endif // __SETTING_H__