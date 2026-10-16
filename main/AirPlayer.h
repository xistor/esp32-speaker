#ifndef __AIR_PLAYER_H__
#define __AIR_PLAYER_H__

#include "esp_err.h"
#include "HAP.h"
inline constexpr const char* AIRPLAY_PROTOCOL_VERSION = "2";
inline constexpr const char* AIRPLAY_SOURCE_VERSION = "377.40.00";

// Flags: 0x4 = audio receiver
inline constexpr const char* AIRPLAY_FLAGS = "0x4";

#ifdef CONFIG_ENABLE_AIRPLAY_ARTWORK
inline constexpr const char* AIRPLAY_METADATA_TYPES = "0,1,2";
#else
inline constexpr const char* AIRPLAY_METADATA_TYPES = "0,2";
#endif

// Model identifier - AudioAccessory for speaker appearance
// AppleTV3,2 = Apple TV, AudioAccessory5,1 = HomePod mini (speaker)
inline constexpr const char* AIRPLAY_MODEL = "AudioAccessory5,1";

inline constexpr const uint32_t AIRPLAY_FEATURES_HI = 0x1C340;
inline constexpr const uint32_t AIRPLAY_FEATURES_LO = 0x405C4A00;

class AirPlayer
{
public:
    AirPlayer(/* args */);
    ~AirPlayer();

    esp_err_t init();
    esp_err_t deinit();

    esp_err_t bonjour();

private:
    constexpr static const char* _TAG = "AirPlayer";

    bool _bonjour_started = false;
    HAP _hap;

};


 


#endif // __AIR_PLAYER_H__