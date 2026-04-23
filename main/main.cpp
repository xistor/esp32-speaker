#include <stdio.h>
#include "esp_log.h"

#include "SpeakerApp.h"


extern "C" void app_main(void)
{
    static SpeakerApp app;


    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)CONFIG_I2S_BCK_PIN,
            .ws = (gpio_num_t)CONFIG_I2S_LRCK_PIN,
            .dout = (gpio_num_t)CONFIG_I2S_DATA_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    app.configureI2s(chan_cfg, std_cfg);

    if (app.init() != ESP_OK) {
        ESP_LOGE("X_SPEAKER", "SpeakerApp initialization failed");
    }
}
