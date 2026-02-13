#include <stdio.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOSConfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* log tags */
#define X_SPEAKER_TAG       "X_SPEAKER"
#define DEVICE_NAME         "X_SPEAKER"

#define APP_DELAY_VALUE                  50  // 5ms

static const char *s_a2d_conn_state_str[] = {"Disconnected", "Connecting", "Connected", "Disconnecting"};
                                             /* connection state in string */
static const char *s_a2d_audio_state_str[] = {"Suspended", "Started"};

enum {
    RINGBUFFER_MODE_PROCESSING,    /* ringbuffer is buffering incoming audio data, I2S is working */
    RINGBUFFER_MODE_PREFETCHING,   /* ringbuffer is buffering incoming audio data, I2S is waiting */
    RINGBUFFER_MODE_DROPPING       /* ringbuffer is not buffering (dropping) incoming audio data, I2S is working */
};

i2s_chan_handle_t tx_chan = NULL;
#define I2S_BCK_PIN     26
#define I2S_LRCK_PIN    27
#define I2S_DATA_PIN    25

static TaskHandle_t s_bt_i2s_task_handle = NULL;  /* handle of I2S task */
static RingbufHandle_t s_ringbuf_i2s = NULL;     /* handle of ringbuffer for I2S */
static SemaphoreHandle_t s_i2s_write_semaphore = NULL;
static uint16_t ringbuffer_mode = RINGBUFFER_MODE_PROCESSING;

#define RINGBUF_HIGHEST_WATER_LEVEL    (64 * 1024)
#define RINGBUF_PREFETCH_WATER_LEVEL   (20 * 1024)

void bt_i2s_driver_install(void)
{

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_PIN,
            .ws = I2S_LRCK_PIN,
            .dout = I2S_DATA_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));


}

void bt_i2s_driver_uninstall(void)
{
    ESP_ERROR_CHECK(i2s_channel_disable(tx_chan));
    ESP_ERROR_CHECK(i2s_del_channel(tx_chan));

}


void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    ESP_LOGI(X_SPEAKER_TAG, "A2DP callback event: %d", event);

    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        ESP_LOGI(X_SPEAKER_TAG, "A2DP connection state changed: %d, [%02x:%02x:%02x:%02x:%02x:%02x]", s_a2d_conn_state_str[param->conn_stat.state],
            param->conn_stat.remote_bda[0], param->conn_stat.remote_bda[1], param->conn_stat.remote_bda[2], param->conn_stat.remote_bda[3], param->conn_stat.remote_bda[4], param->conn_stat.remote_bda[5]);

        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTING) {
            ESP_LOGI(X_SPEAKER_TAG, "A2DP connecting, installing I2S driver");
            bt_i2s_driver_install();
        } else if(param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED)
        {
            ESP_LOGI(X_SPEAKER_TAG, "A2DP connected, set scan mode to non-connectable and non-discoverable, enable I2S channel");
            esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
            ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(X_SPEAKER_TAG, "A2DP disconnected, set scan mode to connectable and discoverable, disable I2S channel");
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            bt_i2s_driver_uninstall();
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(X_SPEAKER_TAG, "A2DP audio state changed: %d", param->audio_stat.state);
        break;
    case ESP_A2D_AUDIO_CFG_EVT:
        ESP_LOGI(X_SPEAKER_TAG, "A2DP audio config changed: codec type %d", param->audio_cfg.mcc.type);
        
        if (param->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
            int sample_rate = 16000;
            int ch_count = 2;
            if (param->audio_cfg.mcc.cie.sbc_info.samp_freq & ESP_A2D_SBC_CIE_SF_32K) {
                sample_rate = 32000;
            } else if (param->audio_cfg.mcc.cie.sbc_info.samp_freq & ESP_A2D_SBC_CIE_SF_44K) {
                sample_rate = 44100;
            } else if (param->audio_cfg.mcc.cie.sbc_info.samp_freq & ESP_A2D_SBC_CIE_SF_48K) {
                sample_rate = 48000;
            }

            if (param->audio_cfg.mcc.cie.sbc_info.ch_mode & ESP_A2D_SBC_CIE_CH_MODE_MONO) {
                ch_count = 1;
            }
            i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
            i2s_std_slot_config_t slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, ch_count);
            i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg);
            i2s_channel_reconfig_std_slot(tx_chan, &slot_cfg);
            ESP_LOGI(X_SPEAKER_TAG, "Configure audio player: 0x%x-0x%x-0x%x-0x%x-0x%x-%d-%d",
                    param->audio_cfg.mcc.cie.sbc_info.samp_freq,
                    param->audio_cfg.mcc.cie.sbc_info.ch_mode,
                    param->audio_cfg.mcc.cie.sbc_info.block_len,
                    param->audio_cfg.mcc.cie.sbc_info.num_subbands,
                    param->audio_cfg.mcc.cie.sbc_info.alloc_mthd,
                    param->audio_cfg.mcc.cie.sbc_info.min_bitpool,
                    param->audio_cfg.mcc.cie.sbc_info.max_bitpool);
            ESP_LOGI(X_SPEAKER_TAG, "Audio player configured, sample rate: %d", sample_rate);
        }

        break;
    case ESP_A2D_PROF_STATE_EVT:
        ESP_LOGI(X_SPEAKER_TAG, "A2DP profile state changed: %d", param->a2d_prof_stat.init_state);
        break;
    case ESP_A2D_SEP_REG_STATE_EVT:
        ESP_LOGI(X_SPEAKER_TAG, "A2DP SEP registration state changed: %d", param->a2d_sep_reg_stat.reg_state);
        break;
    case ESP_A2D_SNK_PSC_CFG_EVT:
        ESP_LOGI(X_SPEAKER_TAG, "protocol service capabilities configured: 0x%x ", param->a2d_psc_cfg_stat.psc_mask);
        if (param->a2d_psc_cfg_stat.psc_mask & ESP_A2D_PSC_DELAY_RPT) {
            ESP_LOGI(X_SPEAKER_TAG, "Peer device support delay reporting");
        } else {
            ESP_LOGI(X_SPEAKER_TAG, "Peer device unsupported delay reporting");
        }
        break;
    case ESP_A2D_SNK_SET_DELAY_VALUE_EVT:
        if (ESP_A2D_SET_INVALID_PARAMS == param->a2d_set_delay_value_stat.set_state) {
            ESP_LOGI(X_SPEAKER_TAG, "Set delay report value: fail");
        } else {
            ESP_LOGI(X_SPEAKER_TAG, "Set delay report value: success, delay_value: %u * 1/10 ms", param->a2d_set_delay_value_stat.delay_value);
        }
        break;
    case ESP_A2D_SNK_GET_DELAY_VALUE_EVT: {
        ESP_LOGI(X_SPEAKER_TAG, "Get delay report value: delay_value: %u * 1/10 ms", param->a2d_get_delay_value_stat.delay_value);
        /* Default delay value plus delay caused by application layer */
        esp_a2d_sink_set_delay_value(param->a2d_get_delay_value_stat.delay_value + APP_DELAY_VALUE);        // bt_app_work_dispatch(bt_av_hdl_a2d_evt, event, param, sizeof(esp_a2d_cb_param_t), NULL);
        break;
    }
    default:
        ESP_LOGE(X_SPEAKER_TAG, "Invalid A2DP event: %d", event);
        break;
    }
}

static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    uint8_t *bda = NULL;

    switch (event) {
    /* when authentication completed, this event comes */
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(X_SPEAKER_TAG, "authentication success: %s", param->auth_cmpl.device_name);
            ESP_LOG_BUFFER_HEX(X_SPEAKER_TAG, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
        } else {
            ESP_LOGE(X_SPEAKER_TAG, "authentication failed, status: %d", param->auth_cmpl.stat);
        }
        ESP_LOGI(X_SPEAKER_TAG, "link key type of current link is: %d", param->auth_cmpl.lk_type);
        break;
    }
    case ESP_BT_GAP_ENC_CHG_EVT: {
        char *str_enc[3] = {"OFF", "E0", "AES"};
        bda = (uint8_t *)param->enc_chg.bda;
        ESP_LOGI(X_SPEAKER_TAG, "Encryption mode to [%02x:%02x:%02x:%02x:%02x:%02x] changed to %s",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5], str_enc[param->enc_chg.enc_mode]);
        break;
    }

    /* when Security Simple Pairing user confirmation requested, this event comes */
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(X_SPEAKER_TAG, "ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %06"PRIu32, param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    /* when Security Simple Pairing passkey notified, this event comes */
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(X_SPEAKER_TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey: %06"PRIu32, param->key_notif.passkey);
        break;
    /* when Security Simple Pairing passkey requested, this event comes */
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(X_SPEAKER_TAG, "ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
        break;

    /* when GAP mode changed, this event comes */
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(X_SPEAKER_TAG, "ESP_BT_GAP_MODE_CHG_EVT mode: %d, interval: %.2f ms",
                 param->mode_chg.mode, param->mode_chg.interval * 0.625);
        break;
    /* when ACL connection completed, this event comes */
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
        bda = (uint8_t *)param->acl_conn_cmpl_stat.bda;
        ESP_LOGI(X_SPEAKER_TAG, "ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT Connected to [%02x:%02x:%02x:%02x:%02x:%02x], status: 0x%x",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5], param->acl_conn_cmpl_stat.stat);
        break;
    /* when ACL disconnection completed, this event comes */
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
        bda = (uint8_t *)param->acl_disconn_cmpl_stat.bda;
        ESP_LOGI(X_SPEAKER_TAG, "ESP_BT_GAP_ACL_DISC_CMPL_STAT_EVT Disconnected from [%02x:%02x:%02x:%02x:%02x:%02x], reason: 0x%x",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5], param->acl_disconn_cmpl_stat.reason);
        break;
    /* others */
    default: {
        ESP_LOGI(X_SPEAKER_TAG, "event: %d", event);
        break;
    }
    }
}

size_t write_ringbuf(const uint8_t *data, size_t size)
{
    size_t item_size = 0;
    BaseType_t done = pdFALSE;

    if (ringbuffer_mode == RINGBUFFER_MODE_DROPPING) {
        ESP_LOGW(X_SPEAKER_TAG, "ringbuffer is full, drop this packet!");
        vRingbufferGetInfo(s_ringbuf_i2s, NULL, NULL, NULL, NULL, &item_size);
        if (item_size <= RINGBUF_PREFETCH_WATER_LEVEL) {
            ESP_LOGI(X_SPEAKER_TAG, "ringbuffer data decreased! mode changed: RINGBUFFER_MODE_PROCESSING");
            ringbuffer_mode = RINGBUFFER_MODE_PROCESSING;
        }
        return 0;
    }

    done = xRingbufferSend(s_ringbuf_i2s, (void *)data, size, (TickType_t)0);

    if (!done) {
        ESP_LOGW(X_SPEAKER_TAG, "ringbuffer overflowed, ready to decrease data! mode changed: RINGBUFFER_MODE_DROPPING");
        ringbuffer_mode = RINGBUFFER_MODE_DROPPING;
    }

    if (ringbuffer_mode == RINGBUFFER_MODE_PREFETCHING) {
        vRingbufferGetInfo(s_ringbuf_i2s, NULL, NULL, NULL, NULL, &item_size);
        if (item_size >= RINGBUF_PREFETCH_WATER_LEVEL) {
            ESP_LOGI(X_SPEAKER_TAG, "ringbuffer data increased! mode changed: RINGBUFFER_MODE_PROCESSING");
            ringbuffer_mode = RINGBUFFER_MODE_PROCESSING;
            if (pdFALSE == xSemaphoreGive(s_i2s_write_semaphore)) {
                ESP_LOGE(X_SPEAKER_TAG, "semphore give failed");
            }
        }
    }

    return done ? size : 0;
}

void bt_app_a2d_data_cb(const uint8_t *data, uint32_t len)
{
    write_ringbuf(data, len);

    static uint32_t s_pkt_cnt = 0;

    /* log the number every 100 packets */
    if (++s_pkt_cnt % 100 == 0) {
        ESP_LOGI(X_SPEAKER_TAG, "Audio packet count: %"PRIu32, s_pkt_cnt);
    }
}




static void bt_i2s_task_handler(void *arg)
{
    uint8_t *data = NULL;
    size_t item_size = 0;
    /**
     * The total length of DMA buffer of I2S is:
     * `dma_frame_num * dma_desc_num * i2s_channel_num * i2s_data_bit_width / 8`.
     * Transmit `dma_frame_num * dma_desc_num` bytes to DMA is trade-off.
     */
    const size_t item_size_upto = 240 * 6;
    size_t bytes_written = 0;

    for (;;) {
        if (pdTRUE == xSemaphoreTake(s_i2s_write_semaphore, portMAX_DELAY)) {
            for (;;) {
                item_size = 0;
                /* receive data from ringbuffer and write it to I2S DMA transmit buffer */
                data = (uint8_t *)xRingbufferReceiveUpTo(s_ringbuf_i2s, &item_size, (TickType_t)pdMS_TO_TICKS(20), item_size_upto);
                if (item_size == 0) {
                    ESP_LOGI(X_SPEAKER_TAG, "ringbuffer underflowed! mode changed: RINGBUFFER_MODE_PREFETCHING");
                    ringbuffer_mode = RINGBUFFER_MODE_PREFETCHING;
                    break;
                }

                i2s_channel_write(tx_chan, data, item_size, &bytes_written, portMAX_DELAY);
                vRingbufferReturnItem(s_ringbuf_i2s, (void *)data);
            }
        }
    }
}

void bt_i2s_task_start_up(void)
{
    ESP_LOGI(X_SPEAKER_TAG, "ringbuffer data empty! mode changed: RINGBUFFER_MODE_PREFETCHING");
    ringbuffer_mode = RINGBUFFER_MODE_PREFETCHING;
    if ((s_i2s_write_semaphore = xSemaphoreCreateBinary()) == NULL) {
        ESP_LOGE(X_SPEAKER_TAG, "%s, Semaphore create failed", __func__);
        return;
    }
    if ((s_ringbuf_i2s = xRingbufferCreate(RINGBUF_HIGHEST_WATER_LEVEL, RINGBUF_TYPE_BYTEBUF)) == NULL) {
        ESP_LOGE(X_SPEAKER_TAG, "%s, ringbuffer create failed", __func__);
        return;
    }
    xTaskCreate(bt_i2s_task_handler, "BtI2STask", 2048, NULL, configMAX_PRIORITIES - 3, &s_bt_i2s_task_handle);
}

void app_main(void)
{
    /* initialize NVS — it is used to store PHY calibration data */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    
    /*
     * This example only uses the functions of Classical Bluetooth.
     * So release the controller memory for Bluetooth Low Energy.
     */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((err = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        ESP_LOGE(X_SPEAKER_TAG, "%s initialize controller failed: %s", __func__, esp_err_to_name(err));
        return;
    }
    if ((err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
        ESP_LOGE(X_SPEAKER_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(err));
        return;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();

    // bluedroid_cfg.ssp_en = false;

    if ((err = esp_bluedroid_init_with_cfg(&bluedroid_cfg)) != ESP_OK) {
        ESP_LOGE(X_SPEAKER_TAG, "%s initialize bluedroid failed: %s", __func__, esp_err_to_name(err));
        return;
    }

    if ((err = esp_bluedroid_enable()) != ESP_OK) {
        ESP_LOGE(X_SPEAKER_TAG, "%s enable bluedroid failed: %s", __func__, esp_err_to_name(err));
        return;
    }

    /* set default parameters for Secure Simple Pairing */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
    
    /* set default parameters for Legacy Pairing (use fixed pin code 1234) */
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_FIXED;
    esp_bt_pin_code_t pin_code;
    pin_code[0] = '0';
    pin_code[1] = '0';
    pin_code[2] = '0';
    pin_code[3] = '0';
    esp_bt_gap_set_pin(pin_type, 4, pin_code);



    esp_bt_gap_set_device_name(DEVICE_NAME);
    // esp_bt_dev_register_callback(bt_app_dev_cb);
    esp_bt_gap_register_callback(bt_app_gap_cb);

    // esp_avrc_ct_register_callback(bt_app_rc_ct_cb);
    // assert(esp_avrc_ct_init() == ESP_OK);
    // esp_avrc_tg_register_callback(bt_app_rc_tg_cb);
    // assert(esp_avrc_tg_init() == ESP_OK);

    // esp_avrc_rn_evt_cap_mask_t evt_set = {0};
    // esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
    // assert(esp_avrc_tg_set_rn_evt_cap(&evt_set) == ESP_OK);

    esp_a2d_register_callback(&bt_app_a2d_cb);

    esp_err_t ret = esp_a2d_sink_register_data_callback(bt_app_a2d_data_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(X_SPEAKER_TAG, "Failed to register A2DP data callback: %s", esp_err_to_name(ret));
    }
    else {
        ESP_LOGI(X_SPEAKER_TAG, "A2DP data callback registered successfully");
    }

    assert(esp_a2d_sink_init() == ESP_OK);

    /* Get the default value of the delay value */
    esp_a2d_sink_get_delay_value();
    /* Get local device name */
    esp_bt_gap_get_device_name();

    /* set discoverable and connectable mode, wait to be connected */
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    

    bt_i2s_task_start_up();
}
