#include "SpeakerApp.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "display/LvglManager.h"
#include <cstring>
#include "esp_spiffs.h"
#include "jpeg_decoder.h"

SpeakerApp *SpeakerApp::s_instance = nullptr;

SpeakerApp::SpeakerApp()
    : _audio_i2s() // default-constructed; configuration later
{
    // the class acts as a singleton since callbacks must be static
    s_instance = this;
}

SpeakerApp::~SpeakerApp()
{
    // Nothing special; controller/bluedroid shutdown could be added here.
}

esp_err_t SpeakerApp::init()
{
    esp_err_t err;

    /* initialize NVS — it is used to store PHY calibration data */
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* release the controller memory for Bluetooth Low Energy. */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    _msg_handler_thread = std::thread(&SpeakerApp::msgHandler, this);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((err = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        ESP_LOGE(_XSPK_TAG, "%s initialize controller failed: %s", __func__, esp_err_to_name(err));
        return err;
    }
    if ((err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
        ESP_LOGE(_XSPK_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(err));
        return err;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    if ((err = esp_bluedroid_init_with_cfg(&bluedroid_cfg)) != ESP_OK) {
        ESP_LOGE(_XSPK_TAG, "%s initialize bluedroid failed: %s", __func__, esp_err_to_name(err));
        return err;
    }

    if ((err = esp_bluedroid_enable()) != ESP_OK) {
        ESP_LOGE(_XSPK_TAG, "%s enable bluedroid failed: %s", __func__, esp_err_to_name(err));
        return err;
    }

    /* set default parameters for Secure Simple Pairing */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));

    /* set default parameters for Legacy Pairing (use fixed pin code 1234) */
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_FIXED;
    esp_bt_pin_code_t pin_code = {'0', '0', '0', '0'};
    esp_bt_gap_set_pin(pin_type, 4, pin_code);

    esp_bt_gap_set_device_name(_device_name);
    esp_bt_gap_register_callback(gapCallback);

    esp_a2d_register_callback(&a2dCallback);
    esp_err_t ret = esp_a2d_sink_register_data_callback(a2dDataCallback);
    if (ret != ESP_OK) {
        ESP_LOGE(_XSPK_TAG, "Failed to register A2DP data callback: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(_XSPK_TAG, "A2DP data callback registered successfully");
    }

    esp_avrc_ct_register_callback(rcCtrlCallback);
    assert(esp_avrc_ct_init() == ESP_OK);
    esp_avrc_tg_register_callback(rcTgCallback);
    assert(esp_avrc_tg_init() == ESP_OK);

    ESP_ERROR_CHECK(esp_a2d_sink_init());

    esp_err_t lv_err = LvglManager::instance().init(CONFIG_LCD_H_RES, CONFIG_LCD_V_RES);
    if (lv_err != ESP_OK) {
        ESP_LOGE(_XSPK_TAG, "LVGL init failed: %s", esp_err_to_name(lv_err));
        return lv_err;
    }


    /* Get the default value of the delay value */
    esp_a2d_sink_get_delay_value();
    /* Get local device name */
    esp_bt_gap_get_device_name();

    /* set discoverable and connectable mode, wait to be connected */
    setScanModeConnectable(true, true);


    while (!LvglManager::instance().isInitialized()) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }


    _ui_music_player.create_ui();

    _ui_music_player.regPlayCtrlCallback(std::bind(&SpeakerApp::playControlCb, this, std::placeholders::_1));

    ESP_LOGI(_XSPK_TAG, "UI created successfully");
    return ESP_OK;
}

void SpeakerApp::configureI2s(const i2s_chan_config_t &chan_cfg,
                              const i2s_std_config_t &std_cfg)
{
    _audio_i2s.configI2s(chan_cfg, std_cfg);
}

/* static callbacks */
void SpeakerApp::a2dCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    if (s_instance){
        s_instance->msgDispatch([event](uint16_t evt, void *p) {
            s_instance->handleA2dpEvent(event, (esp_a2d_cb_param_t *)p);
        }, event, param, sizeof(esp_a2d_cb_param_t));
    }

}

void SpeakerApp::gapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (s_instance){
        s_instance->msgDispatch([event](uint16_t evt, void *p) {
            s_instance->handleGapEvent(event, (esp_bt_gap_cb_param_t *)p);
        }, event, param, sizeof(esp_bt_gap_cb_param_t));
    }
}

void SpeakerApp::a2dDataCallback(const uint8_t *data, uint32_t len)
{
    if (s_instance) {
        s_instance->handleA2dpData(data, len);
    }
}

void SpeakerApp::rcCtrlCallback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch(event) {
        case ESP_AVRC_CT_METADATA_RSP_EVT:
            s_instance->msgDispatch([event](uint16_t evt, void *p) {
                        s_instance->handleRcCtrlEvent(event, (esp_avrc_ct_cb_param_t *)p);
                    }, event, param, sizeof(esp_avrc_ct_cb_param_t), SpeakerApp::avrcCommonnCopyMetaData, SpeakerApp::avrcCommonFreeMetaData);
            break;
        case ESP_AVRC_CT_COVER_ART_DATA_EVT:

            if (param->cover_art_data.status == ESP_BT_STATUS_SUCCESS) {
                s_instance->saveCoverImageData(param->cover_art_data.p_data, param->cover_art_data.data_len);
            } else {
                ESP_LOGW(_XSPK_TAG, "Cover Art Client get operation failed");
                break;
            }
            s_instance->msgDispatch([event](uint16_t evt, void *p) {
                        s_instance->handleRcCtrlEvent(event, (esp_avrc_ct_cb_param_t *)p);
                    }, event, param, sizeof(esp_avrc_ct_cb_param_t));
            break;
        case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
        case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        case ESP_AVRC_CT_COVER_ART_STATE_EVT:
        case ESP_AVRC_CT_PROF_STATE_EVT:
            s_instance->msgDispatch([event](uint16_t evt, void *p) {
                        s_instance->handleRcCtrlEvent(event, (esp_avrc_ct_cb_param_t *)p);
                    }, event, param, sizeof(esp_avrc_ct_cb_param_t));
            break;
        default:
            break;
    }
        
}

void SpeakerApp::rcTgCallback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    if (s_instance){
        s_instance->msgDispatch([event](uint16_t evt, void *p) {
            s_instance->handleRcTgEvent(event, (esp_avrc_tg_cb_param_t *)p);
        }, event, param, sizeof(esp_avrc_tg_cb_param_t));
    }
}

/* instance methods */
void SpeakerApp::setScanModeConnectable(bool conn, bool discoverable)
{
    esp_bt_gap_set_scan_mode(conn ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE,
                              discoverable ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
}

void SpeakerApp::handleA2dpEvent(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    static const char *state_str[] = {"Disconnected", "Connecting", "Connected", "Disconnecting"};
    ESP_LOGI(_XSPK_TAG, "A2DP callback event: %d", event);

    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        ESP_LOGI(_XSPK_TAG, "A2DP connection state changed: %s, [%02x:%02x:%02x:%02x:%02x:%02x]",
                 state_str[param->conn_stat.state],
                 param->conn_stat.remote_bda[0], param->conn_stat.remote_bda[1], param->conn_stat.remote_bda[2],
                 param->conn_stat.remote_bda[3], param->conn_stat.remote_bda[4], param->conn_stat.remote_bda[5]);

        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTING) {
            ESP_LOGI(_XSPK_TAG, "A2DP connecting, installing I2S driver");
            AudioI2sError err = _audio_i2s.start();
            if( err != AudioI2sError::NONE) {
                ESP_LOGE(_XSPK_TAG, "Failed to start audio i2s err: %d", static_cast<int>(err));
            }
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(_XSPK_TAG, "A2DP connected, set scan mode to non-connectable and non-discoverable, enable I2S channel");
            setScanModeConnectable(false, false);
            _audio_i2s.enableI2s();
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(_XSPK_TAG, "A2DP disconnected, set scan mode to connectable and discoverable, disable I2S channel");
            setScanModeConnectable(true, true);
            _audio_i2s.stop();
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(_XSPK_TAG, "A2DP audio state changed: %d", param->audio_stat.state);
        break;
    case ESP_A2D_AUDIO_CFG_EVT:
        ESP_LOGI(_XSPK_TAG, "A2DP audio config changed: codec type %d", param->audio_cfg.mcc.type);
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
            i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)sample_rate);
            i2s_std_slot_config_t slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                                              (i2s_slot_mode_t)ch_count);

            _audio_i2s.reConfigI2s(clk_cfg, slot_cfg);
            ESP_LOGI(_XSPK_TAG, "Configure audio player: 0x%x-0x%x-0x%x-0x%x-0x%x-%d-%d",
                     param->audio_cfg.mcc.cie.sbc_info.samp_freq,
                     param->audio_cfg.mcc.cie.sbc_info.ch_mode,
                     param->audio_cfg.mcc.cie.sbc_info.block_len,
                     param->audio_cfg.mcc.cie.sbc_info.num_subbands,
                     param->audio_cfg.mcc.cie.sbc_info.alloc_mthd,
                     param->audio_cfg.mcc.cie.sbc_info.min_bitpool,
                     param->audio_cfg.mcc.cie.sbc_info.max_bitpool);
            ESP_LOGI(_XSPK_TAG, "Audio player configured, sample rate: %d", sample_rate);
        }
        break;
    case ESP_A2D_PROF_STATE_EVT:
        ESP_LOGI(_XSPK_TAG, "A2DP profile state changed: %d", param->a2d_prof_stat.init_state);
        break;
    case ESP_A2D_SEP_REG_STATE_EVT:
        ESP_LOGI(_XSPK_TAG, "A2DP SEP registration state changed: %d", param->a2d_sep_reg_stat.reg_state);
        break;
    case ESP_A2D_SNK_PSC_CFG_EVT:
        ESP_LOGI(_XSPK_TAG, "protocol service capabilities configured: 0x%x ", param->a2d_psc_cfg_stat.psc_mask);
        if (param->a2d_psc_cfg_stat.psc_mask & ESP_A2D_PSC_DELAY_RPT) {
            ESP_LOGI(_XSPK_TAG, "Peer device support delay reporting");
        } else {
            ESP_LOGI(_XSPK_TAG, "Peer device unsupported delay reporting");
        }
        break;
    case ESP_A2D_SNK_SET_DELAY_VALUE_EVT:
        if (ESP_A2D_SET_INVALID_PARAMS == param->a2d_set_delay_value_stat.set_state) {
            ESP_LOGI(_XSPK_TAG, "Set delay report value: fail");
        } else {
            ESP_LOGI(_XSPK_TAG, "Set delay report value: success, delay_value: %u * 1/10 ms",
                     param->a2d_set_delay_value_stat.delay_value);
        }
        break;
    case ESP_A2D_SNK_GET_DELAY_VALUE_EVT: {
        ESP_LOGI(_XSPK_TAG, "Get delay report value: delay_value: %u * 1/10 ms",
                 param->a2d_get_delay_value_stat.delay_value);
        /* Default delay value plus delay caused by application layer */
        esp_a2d_sink_set_delay_value(param->a2d_get_delay_value_stat.delay_value + APP_DELAY_VALUE);
        break;
    }
    default:
        ESP_LOGE(_XSPK_TAG, "Invalid A2DP event: %d", event);
        break;
    }
}

void SpeakerApp::handleGapEvent(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    uint8_t *bda = nullptr;

    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(_XSPK_TAG, "authentication success: %s", param->auth_cmpl.device_name);
            ESP_LOG_BUFFER_HEX(_XSPK_TAG, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
        } else {
            ESP_LOGE(_XSPK_TAG, "authentication failed, status: %d", param->auth_cmpl.stat);
        }
        ESP_LOGI(_XSPK_TAG, "link key type of current link is: %d", param->auth_cmpl.lk_type);
        break;
    }
    case ESP_BT_GAP_ENC_CHG_EVT: {
        const char *str_enc[3] = {"OFF", "E0", "AES"};
        bda = (uint8_t *)param->enc_chg.bda;
        ESP_LOGI(_XSPK_TAG, "Encryption mode to [%02x:%02x:%02x:%02x:%02x:%02x] changed to %s",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5], str_enc[param->enc_chg.enc_mode]);
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(_XSPK_TAG, "ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %06" PRIu32,
                 param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(_XSPK_TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey: %06" PRIu32, param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(_XSPK_TAG, "ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
        break;
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(_XSPK_TAG, "ESP_BT_GAP_MODE_CHG_EVT mode: %d, interval: %.2f ms",
                 param->mode_chg.mode, param->mode_chg.interval * 0.625);
        break;
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
        bda = (uint8_t *)param->acl_conn_cmpl_stat.bda;
        ESP_LOGI(_XSPK_TAG, "ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT Connected to [%02x:%02x:%02x:%02x:%02x:%02x], status: 0x%x",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5], param->acl_conn_cmpl_stat.stat);
        break;
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
        bda = (uint8_t *)param->acl_disconn_cmpl_stat.bda;
        ESP_LOGI(_XSPK_TAG, "ESP_BT_GAP_ACL_DISC_CMPL_STAT_EVT Disconnected from [%02x:%02x:%02x:%02x:%02x:%02x], reason: 0x%x",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5], param->acl_disconn_cmpl_stat.reason);
        break;
    default:
        ESP_LOGI(_XSPK_TAG, "event: %d", event);
        break;
    }
}

void SpeakerApp::handleA2dpData(const uint8_t *data, uint32_t len)
{
    _audio_i2s.sendToI2s(data, len);

    static uint32_t s_pkt_cnt = 0;
    if (++s_pkt_cnt % 100 == 0) {
        ESP_LOGI(_XSPK_TAG, "Audio packet count: %" PRIu32, s_pkt_cnt);
    }
}

void SpeakerApp::handleRcCtrlEvent(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    ESP_LOGI(_XSPK_TAG, "AVRCP Controller event: %d", event);



    // Handle AVRCP controller events here (e.g., connection state changes, passthrough responses, etc.)
    switch (event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT: {
            if (param->conn_stat.connected) {
                ESP_LOGI(_XSPK_TAG, "AVRCP connected...");
                esp_avrc_ct_send_metadata_cmd(allocTransactionLabel(), ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM);
                esp_avrc_ct_send_register_notification_cmd(allocTransactionLabel(), ESP_AVRC_RN_TRACK_CHANGE, 0);
                esp_avrc_ct_send_register_notification_cmd(allocTransactionLabel(), ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
                esp_avrc_ct_send_register_notification_cmd(allocTransactionLabel(), ESP_AVRC_RN_PLAY_POS_CHANGED, 3);

                esp_avrc_ct_send_get_rn_capabilities_cmd(allocTransactionLabel());
            }
            break;
        }
        case ESP_AVRC_CT_METADATA_RSP_EVT: {
            char *text = (char *)malloc(param->meta_rsp.attr_length + 1);
            memcpy(text, param->meta_rsp.attr_text, param->meta_rsp.attr_length);
            text[param->meta_rsp.attr_length] = '\0';

            if (param->meta_rsp.attr_id == ESP_AVRC_MD_ATTR_TITLE) {
                ESP_LOGI(_XSPK_TAG, "%s", text);
                _ui_music_player.setTitle(text);
            } else if (param->meta_rsp.attr_id == ESP_AVRC_MD_ATTR_ARTIST) {
                ESP_LOGI(_XSPK_TAG, "Artist: %s", text);
                _ui_music_player.setArtist(text);
            } else if (param->meta_rsp.attr_id == ESP_AVRC_MD_ATTR_PLAYING_TIME) {
                char *playtime_str = (char *)param->meta_rsp.attr_text;
                uint32_t playtime_ms = atoi(playtime_str);

                _ui_music_player.setPlayTime(playtime_ms);

            } else if (param->meta_rsp.attr_id == ESP_AVRC_MD_ATTR_COVER_ART) {

                if(memcmp(_cover_image_handler, param->meta_rsp.attr_text, 7) != 0) {
                    ESP_LOGI(_XSPK_TAG, "New cover art image handler received: %s, len: %d", param->meta_rsp.attr_text, param->meta_rsp.attr_length);
                    memcpy(_cover_image_handler, param->meta_rsp.attr_text, 7);
                    // esp_avrc_ct_cover_art_get_image_properties(_cover_image_handler);
                    esp_avrc_ct_cover_art_get_linked_thumbnail(_cover_image_handler);
                    _cover_image_size = 0;
                    if(_cover_image_data) {
                        free(_cover_image_data);
                        _cover_image_data = nullptr;
                    }
                } else {
                    ESP_LOGI(_XSPK_TAG, "Same cover art image handler received, ignoring: %s", param->meta_rsp.attr_text);
                }

            }

            free(text);
            break;
        }
        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT: {
            switch(param->change_ntf.event_id) {
                case ESP_AVRC_RN_TRACK_CHANGE:
                    esp_avrc_ct_send_metadata_cmd(allocTransactionLabel(), ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_PLAYING_TIME);
                    esp_avrc_ct_send_register_notification_cmd(allocTransactionLabel(), ESP_AVRC_RN_TRACK_CHANGE, 0);
                    esp_avrc_ct_send_metadata_cmd(allocTransactionLabel(), ESP_AVRC_MD_ATTR_COVER_ART);
                    break;
                case ESP_AVRC_RN_PLAY_STATUS_CHANGE:
                    ESP_LOGI(_XSPK_TAG, "Playback status changed: 0x%x", param->change_ntf.event_parameter.playback);
                    if( param->change_ntf.event_parameter.playback == ESP_AVRC_PLAYBACK_PLAYING) {
                        _ui_music_player.setPlaying(true);
                    } else {
                        _ui_music_player.setPlaying(false);
                    }
                    esp_avrc_ct_send_register_notification_cmd(allocTransactionLabel(), ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
                    break;
                case ESP_AVRC_RN_PLAY_POS_CHANGED:
                    ESP_LOGI(_XSPK_TAG, "Play position changed: %"PRIu32"ms", param->change_ntf.event_parameter.play_pos);
                    _ui_music_player.setPlayPosition(param->change_ntf.event_parameter.play_pos);
                    esp_avrc_ct_send_register_notification_cmd(allocTransactionLabel(), ESP_AVRC_RN_PLAY_POS_CHANGED, 3);

                    break;
                default:
                    ESP_LOGI(_XSPK_TAG, "Other notification received, event_id: %d", param->change_ntf.event_id);
                    break;
            }

            break;
        }
        case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
            /* request the cover art */
            esp_avrc_ct_send_metadata_cmd(allocTransactionLabel(), ESP_AVRC_MD_ATTR_COVER_ART);
            break;
        }
        case ESP_AVRC_CT_REMOTE_FEATURES_EVT: {
            ESP_LOGI(_XSPK_TAG, "AVRC remote features %" PRIx32", TG features %x", param->rmt_feats.feat_mask, param->rmt_feats.tg_feat_flag);
            if ((param->rmt_feats.tg_feat_flag & ESP_AVRC_FEAT_FLAG_TG_COVER_ART)) {
                ESP_LOGW(_XSPK_TAG, "Peer support Cover Art feature");
                /* start the cover art connection */
                esp_avrc_ct_cover_art_connect(1024);
            }
            break;
        }
        case ESP_AVRC_CT_COVER_ART_STATE_EVT: {
            if (param->cover_art_state.state == ESP_AVRC_COVER_ART_CONNECTED) {
                ESP_LOGW(_XSPK_TAG, "Cover Art Client connected");
                /* request the cover art */
                esp_avrc_ct_send_metadata_cmd(allocTransactionLabel(), ESP_AVRC_MD_ATTR_COVER_ART);
            } else {
                ESP_LOGW(_XSPK_TAG, "Cover Art Client disconnected, reason:%d", param->cover_art_state.reason);
            }
            break;
        }
        case ESP_AVRC_CT_COVER_ART_DATA_EVT:
        {
            esp_err_t ret = ESP_OK;


            if ( param->cover_art_data.final) {
                ESP_LOGI(_XSPK_TAG, "Final cover art data received, total size: %d", _cover_image_size);

                /* decode and display the image */
                if(_cover_pixels) {
                    free(_cover_pixels);
                    _cover_pixels = nullptr;
                }
                _cover_pixels = (uint16_t *)calloc(200 * 200, sizeof(uint16_t));

                /* JPEG decode config */
                esp_jpeg_image_cfg_t jpeg_cfg = {
                    .indata = (uint8_t *)_cover_image_data,
                    .indata_size = _cover_image_size,
                    .outbuf = (uint8_t*)(_cover_pixels),
                    .outbuf_size = 200 * 200 * sizeof(uint16_t),
                    .out_format = JPEG_IMAGE_FORMAT_RGB565,
                    .out_scale = JPEG_IMAGE_SCALE_0,
                    .flags = {
                        .swap_color_bytes = 1,
                    }
                };

                /* JPEG decode */
                esp_jpeg_image_output_t outimg;
                ret = esp_jpeg_decode(&jpeg_cfg, &outimg);
                /* display the image */

                if (ret == ESP_OK) {
                    ESP_LOGI(_XSPK_TAG, "JPEG decode successful, image size: %d x %d", outimg.width, outimg.height);
                    _ui_music_player.setCoverArt(( uint8_t *)_cover_pixels);
                } else {
                    ESP_LOGE(_XSPK_TAG, "JPEG decode failed: %s", esp_err_to_name(ret));
                }
            }

            break;
        }

        default:
            break;
    }
}

void SpeakerApp::handleRcTgEvent(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    ESP_LOGI(_XSPK_TAG, "AVRCP Target event: %d", event);
    // Handle AVRCP target events here (e.g., connection state changes, passthrough responses, etc.)
}

bool SpeakerApp::msgDispatch(BtAppCallback callback, uint16_t event, void *p_params, int param_len, BtAppCopyCallback copy_callback, DeepFreeCallback free_callback)
{
    if (s_instance) {
        bt_app_msg_t msg;
        memset(&msg, 0, sizeof(msg));

        msg.event = event;
        msg.callback = callback;


        if (param_len == 0) {
            s_instance->pushBtMsg(msg);
            return true;
        } else if (p_params && param_len > 0) {
            if ((msg.param = malloc(param_len)) != NULL) {
                memcpy(msg.param, p_params, param_len);
                if(copy_callback)
                {
                    copy_callback(msg.param, p_params, param_len);
                }
                
                if(free_callback)
                {
                    msg.free_callback = free_callback;
                }
                s_instance->pushBtMsg(msg);
                return true;
            }
        }
    }
    return false;

}

void SpeakerApp::msgHandler() {
    while (true) {
        bt_app_msg_t msg = _bt_msg_queue.take();

        ESP_LOGI(_XSPK_TAG, "Processing Bluetooth event: %d", msg.event);
        if(msg.callback)
        {
            msg.callback(msg.event, msg.param);
        }

        if (msg.param) {
            free(msg.param);
        }

    }
}

void SpeakerApp::pushBtMsg(const bt_app_msg_t &msg) {
    _bt_msg_queue.put(msg);
}

void SpeakerApp::avrcCommonnCopyMetaData(void *p_dest, void *p_src, int len)
{
    if (p_dest == nullptr || p_src == nullptr) {
        ESP_LOGE("SPEAKER_APP", "avrcCommonnCopyMetaData: Blocked null pointer structure map!");
        return;
    }

    esp_avrc_ct_cb_param_t *p_dest_rc = (esp_avrc_ct_cb_param_t *)(p_dest);
    esp_avrc_ct_cb_param_t *p_src_rc = (esp_avrc_ct_cb_param_t *)(p_src);

    p_dest_rc->meta_rsp.attr_id = p_src_rc->meta_rsp.attr_id;
    p_dest_rc->meta_rsp.attr_length = p_src_rc->meta_rsp.attr_length;

    // ESP_LOGI("SPEAKER_APP", "avrcCommonnCopyMetaData: Copying metadata attr_id=%d, attr_length=%d", p_src_rc->meta_rsp.attr_id, p_src_rc->meta_rsp.attr_length);
    uint8_t *p_attr_text = (uint8_t *) malloc(p_dest_rc->meta_rsp.attr_length + 1);
    if (p_attr_text == nullptr) {
        ESP_LOGE("SPEAKER_APP", "avrcCommonnCopyMetaData: Failed to allocate memory for attr_text!");
        return;
    }
    p_dest_rc->meta_rsp.attr_text = p_attr_text;

    memcpy(p_dest_rc->meta_rsp.attr_text, p_src_rc->meta_rsp.attr_text, p_dest_rc->meta_rsp.attr_length);
    p_dest_rc->meta_rsp.attr_text[p_dest_rc->meta_rsp.attr_length] = 0;
}

void SpeakerApp::avrcCommonFreeMetaData(void *ptr)
{
    if (ptr == nullptr) {
        ESP_LOGE("SPEAKER_APP", "avrcCommonFreeMetaData: Blocked null pointer free!");
        return;
    }

    esp_avrc_ct_cb_param_t *p_rc = (esp_avrc_ct_cb_param_t *)(ptr);
    if (p_rc->meta_rsp.attr_text) {
        free(p_rc->meta_rsp.attr_text);
        p_rc->meta_rsp.attr_text = nullptr;
    }
}

void SpeakerApp::saveCoverImageData(const uint8_t *data, uint32_t data_len)
{
   _cover_image_size += data_len;

    ESP_LOGI(_XSPK_TAG, "Received cover art data: %d", data_len);

    uint8_t *p_buf = (uint8_t *)realloc(_cover_image_data, _cover_image_size * sizeof(uint8_t));
    if (!p_buf) {
        ESP_LOGE(_XSPK_TAG, "%s: The memory allocation of Cover art image data failed", __func__);
        if (_cover_image_data) {
            free(_cover_image_data);
            _cover_image_data = nullptr;
        }
        return;
    }
    _cover_image_data = p_buf;
    memcpy(_cover_image_data + _cover_image_size - data_len, data, data_len);
}

uint8_t SpeakerApp::allocTransactionLabel()
{
    static uint8_t tl = 0;
    if (tl > ESP_AVRC_TRANS_LABEL_MAX) {
        tl = 0;
    }

    return tl++;
}

void SpeakerApp::playControlCb(UiMusicPlayer::play_ctrl_param_t ctrl_param)
{
    // ESP_LOGI(_XSPK_TAG, "Prev/Next control callback, ctrl cmd: %d\n", ctrl_param.cmd );

    if (s_instance){
        s_instance->msgDispatch([ctrl_param](uint16_t evt, void *p) {
            s_instance->handlePlayControl(evt, (UiMusicPlayer::play_ctrl_param_t *)p);
        }, 0, &ctrl_param, sizeof(UiMusicPlayer::play_ctrl_param_t));
    }

}

void SpeakerApp::handlePlayControl(uint16_t evt, UiMusicPlayer::play_ctrl_param_t *ctrl_param)
{
    if(ctrl_param->cmd == UiMusicPlayer::playControlCmd::PLAY) {
        ESP_LOGI(_XSPK_TAG, "Play command received");
        esp_avrc_ct_send_passthrough_cmd(allocTransactionLabel(), ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_PRESSED);
        esp_avrc_ct_send_passthrough_cmd(allocTransactionLabel(), ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_RELEASED);
    } else if(ctrl_param->cmd == UiMusicPlayer::playControlCmd::PAUSE) {
        ESP_LOGI(_XSPK_TAG, "Pause command received");
        esp_avrc_ct_send_passthrough_cmd(allocTransactionLabel(), ESP_AVRC_PT_CMD_PAUSE, ESP_AVRC_PT_CMD_STATE_PRESSED);
        esp_avrc_ct_send_passthrough_cmd(allocTransactionLabel(), ESP_AVRC_PT_CMD_PAUSE, ESP_AVRC_PT_CMD_STATE_RELEASED);
    } else if (ctrl_param->cmd == UiMusicPlayer::playControlCmd::PREVIOUS) {
        ESP_LOGI(_XSPK_TAG, "Previous command received");
        esp_avrc_ct_send_passthrough_cmd(allocTransactionLabel(), ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_STATE_PRESSED);
        esp_avrc_ct_send_passthrough_cmd(allocTransactionLabel(), ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_STATE_RELEASED);
    } else if (ctrl_param->cmd == UiMusicPlayer::playControlCmd::NEXT) {
        ESP_LOGI(_XSPK_TAG, "Next command received");
        esp_avrc_ct_send_passthrough_cmd(allocTransactionLabel(), ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_STATE_PRESSED);
        esp_avrc_ct_send_passthrough_cmd(allocTransactionLabel(), ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_STATE_RELEASED); 
    }
}