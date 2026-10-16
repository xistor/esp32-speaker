#include "AirPlayer.h"
#include "WifiMgr.h"
#include "esp_log.h"
#include "mdns.h"
#include <cstdio>
#include <cinttypes>

AirPlayer::AirPlayer(/* args */)
{
}

AirPlayer::~AirPlayer()
{
}

esp_err_t AirPlayer::init()
{
    ESP_ERROR_CHECK(_hap.init());

    return ESP_OK;
}

esp_err_t AirPlayer::deinit()
{
    if (_bonjour_started) {
        mdns_free();
        _bonjour_started = false;
    }
    return ESP_OK;
}

esp_err_t AirPlayer::bonjour()
{

    if (_bonjour_started) {
        return ESP_OK;
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(_TAG, "Failed to initialize mDNS: %s", esp_err_to_name(err));
        return err;
    }

    const std::string mac_str = WifiMgr::instance().getMacAddrStr();
    if (mac_str.empty()) {
        ESP_LOGE(_TAG, "Cannot advertise AirPlay without a Wi-Fi MAC address");
        if (err == ESP_OK) {
            mdns_free();
        }
        return ESP_ERR_INVALID_STATE;
    }

    auto  pk = _hap.getPublicKey();
    char pk_str[65];
    for (size_t i = 0; i < pk.size(); ++i) {
        snprintf(pk_str + i * 2, sizeof(pk_str) - i * 2, "%02X", pk[i]);
    }

    char features_str[32];

    // Format features as "hi,lo" hex string
    snprintf(features_str, sizeof(features_str), "0x%" PRIx32 ",0x%" PRIx32, AIRPLAY_FEATURES_LO,
            AIRPLAY_FEATURES_HI);

    // The hostname is used for the mDNS A record; the configured name remains
    // the user-visible service instance name.
    err = mdns_hostname_set(CONFIG_SPEAKER_DEVICE_NAME);
    if (err != ESP_OK) {
        ESP_LOGE(_TAG, "Failed to set mDNS hostname: %s", esp_err_to_name(err));
        return err;
    }
    err = mdns_instance_name_set(CONFIG_SPEAKER_DEVICE_NAME);
    if (err != ESP_OK) {
        ESP_LOGE(_TAG, "Failed to set mDNS instance name: %s", esp_err_to_name(err));
        return err;
    }

    char raop_instance[128];
    snprintf(raop_instance, sizeof(raop_instance), "%s@%s",
             mac_str.c_str(), CONFIG_SPEAKER_DEVICE_NAME);

     mdns_txt_item_t airplay_txt[] = {
        {"deviceid", mac_str.c_str()},
        {"features", features_str},
        {"flags", AIRPLAY_FLAGS},
        {"model", AIRPLAY_MODEL},
        {"pk", pk_str},
        {"pi", "00000000-0000-0000-0000-000000000000"}, // Pairing identity UUID
        {"srcvers", AIRPLAY_SOURCE_VERSION},
        {"vv", AIRPLAY_PROTOCOL_VERSION},
        {"acl", "0"},
    };

    err = mdns_service_add(CONFIG_SPEAKER_DEVICE_NAME, "_airplay", "_tcp",
                           7000, airplay_txt,
                           sizeof(airplay_txt) / sizeof(airplay_txt[0]));
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(_TAG, "Failed to add _airplay service: %s", esp_err_to_name(err));
        return err;
    }

    mdns_txt_item_t raop_txt[] = {
        {"am", AIRPLAY_MODEL},
        {"cn", "0,1,2,3"},              // Audio codecs: PCM, ALAC, AAC, AAC-ELD
        {"da", "true"},                 // Digest auth
        {"ek", "1"},                    // Encryption key available (RSA)
        {"et", "0,1,3,5"},              // Encryption types
        {"ft", features_str},           // Features (same as airplay)
        {"md", AIRPLAY_METADATA_TYPES}, // Metadata types
        {"pk", pk_str},                 // Public key
        {"sf", AIRPLAY_FLAGS},          // Status flags
        {"tp", "UDP"},                  // Transport protocol
        {"vn", "65537"},                // Version number
        {"vs", AIRPLAY_SOURCE_VERSION},
        {"vv", AIRPLAY_PROTOCOL_VERSION},
    };

    err = mdns_service_add(raop_instance, "_raop", "_tcp", 7000, raop_txt,
                           sizeof(raop_txt) / sizeof(raop_txt[0]));
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(_TAG, "Failed to add _raop service: %s", esp_err_to_name(err));
        return err;
    }

    _bonjour_started = true;
    ESP_LOGI(_TAG, " '%s'", CONFIG_SPEAKER_DEVICE_NAME);
    return ESP_OK;
}