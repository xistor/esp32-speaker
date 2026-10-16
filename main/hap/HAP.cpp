#include "HAP.h"
#include "NvsHelper.h"
#include "sodium.h"
#include "esp_log.h"

esp_err_t HAP::init(void) {

    esp_err_t err =  ESP_OK;

    if (_initalized) {
        return ESP_OK;
    }

    if(sodium_init() < 0) {
        ESP_LOGE(_TAG, "Failed to initialize libsodium");
        return ESP_FAIL;
    }

    err = NvsHelper::load(AIRPLAY_NVS_NAMESPACE, AIRPLAY_NVS_KEY_PUBLIC,
        _device_public_key, HAP_ED25519_PUBLIC_KEY_SIZE);
    if(err == ESP_OK) {
        err =  NvsHelper::load(AIRPLAY_NVS_NAMESPACE, AIRPLAY_NVS_KEY_SECRET,
            _device_secret_key, HAP_ED25519_SECRET_KEY_SIZE);
    }

    if(err != ESP_OK) {
        crypto_sign_keypair(_device_public_key, _device_secret_key);
        err = NvsHelper::save(AIRPLAY_NVS_NAMESPACE, AIRPLAY_NVS_KEY_PUBLIC,
            _device_public_key, HAP_ED25519_PUBLIC_KEY_SIZE);
        if(err != ESP_OK) {
            ESP_LOGE(_TAG, "Failed to save public key to NVS");
            return err;
        }

        err = NvsHelper::save(AIRPLAY_NVS_NAMESPACE, AIRPLAY_NVS_KEY_SECRET,
            _device_secret_key, HAP_ED25519_SECRET_KEY_SIZE);
        if(err != ESP_OK) {
            ESP_LOGE(_TAG, "Failed to save secret key to NVS");
            return err;
        }

    }


    _initalized = true;
    return ESP_OK;
}

