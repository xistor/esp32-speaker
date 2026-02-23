#pragma once

#include <memory>
#include "AudioI2s.h"
#include "esp_a2dp_api.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"


#define APP_DELAY_VALUE 50  // 5ms

/**
 * 
 *
 * Call `init()` after constructing an instance.  Configuration of the
 * I2S peripheral happens via `configureI2s()` before the first
 * connection attempt.
 */
class SpeakerApp {
public:
    SpeakerApp();
    ~SpeakerApp();

    /**
     * Initialize Bluetooth stack, register callbacks, etc.
     * Returns ESP_OK on success.
     */
    esp_err_t init();

    /**
     * Configure the I2S driver (can be called multiple times).
     */
    void configureI2s(const i2s_chan_config_t &chan_cfg,
                      const i2s_std_config_t &std_cfg);

private:
    // Bluetooth callbacks have to be static, so they route to the
    // singleton instance.
    static void a2dCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
    static void gapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
    static void a2dDataCallback(const uint8_t *data, uint32_t len);

    void handleA2dpEvent(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
    void handleGapEvent(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
    void handleA2dpData(const uint8_t *data, uint32_t len);

    // internal helpers
    void setScanModeConnectable(bool conn, bool discoverable);

    AudioI2s _audio_i2s;
    const char *_device_name = CONFIG_SPAKER_DEVICE_NAME;

    static SpeakerApp *s_instance;
};
