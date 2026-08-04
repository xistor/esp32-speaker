#include "AudioDecoder.h"
#include "esp_pthread.h"


AudioDecoder::AudioDecoder()
{
    _audio_raw_data_mux = xSemaphoreCreateMutex();

}

AudioDecoder::~AudioDecoder()
{
    if (_audio_raw_data_mux) {
        vSemaphoreDelete(_audio_raw_data_mux);
        _audio_raw_data_mux = nullptr;
    }

}

void AudioDecoder::start()
{
    _running.store(true);

    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.thread_name = "i2STask";
    cfg.pin_to_core = 0; 
    cfg.prio = 15; 

    esp_pthread_set_cfg(&cfg);

    _decode_task_thread = std::thread(&AudioDecoder::audio_decode_task, this);

}

void AudioDecoder::stop()
{
    _running.store(false);
    _a2dp_raw_queue.put(nullptr); // unblock the take() call in audio_decode_task()

    if (_decode_task_thread.joinable()) {
        _decode_task_thread.join();
    }
}

void AudioDecoder::decoderDataFlush()
{
    // Drain the queue and free the audio buffers
    auto drained_buffers = _a2dp_raw_queue.drain();
    for (auto buffer : drained_buffers) {
        esp_a2d_audio_buff_free(buffer);
    }
}

void AudioDecoder::decoderClose()
{
    if(_dec_opened.load()) {
        esp_audio_dec_close(_dec_hdl);
        _dec_hdl = nullptr;
        _dec_opened.store(false);
    }
}

void AudioDecoder::pushAudioData(esp_a2d_audio_buff_t *audio_buf)
{
    if (audio_buf == nullptr) {
        return;
    }

    _a2dp_raw_queue.put(audio_buf);
 
}

esp_audio_err_t AudioDecoder::applyMcc(const esp_a2d_mcc_t *mcc)
{
    esp_audio_err_t ret;

    if (mcc == NULL) {
        return ESP_AUDIO_ERR_INVALID_PARAMETER;
    }

    if (mcc->type != ESP_A2D_MCT_SBC && mcc->type != ESP_A2D_MCT_M24) {
        ESP_LOGW(_DECODER_TAG, "Unsupported A2DP codec type: %d", mcc->type);
        return ESP_AUDIO_ERR_FAIL;
    }

    decoderClose();

    if (mcc->type == ESP_A2D_MCT_SBC) {
        esp_sbc_dec_cfg_t sbc_cfg = ESP_SBC_DEC_CONFIG_DEFAULT();
        sbc_cfg.sbc_mode = ESP_SBC_MODE_STD;
        sbc_cfg.ch_num = getChannelCount(mcc);
        sbc_cfg.enable_plc = true;

        esp_audio_dec_cfg_t dec_cfg = {
            .type = ESP_AUDIO_TYPE_SBC,
            .cfg = &sbc_cfg,
            .cfg_sz = sizeof(esp_sbc_dec_cfg_t),
        };

        esp_sbc_dec_register();
        ret = esp_audio_dec_open(&dec_cfg, &_dec_hdl);
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(_DECODER_TAG, "esp_audio_dec_open (SBC) failed: %d", ret);
            return ret;
        }
        ESP_LOGI(_DECODER_TAG, "SBC decoder open: %" PRIu32 " Hz, %d ch",
                 (uint32_t)getSampleRate(mcc), sbc_cfg.ch_num);
    } else {
        const esp_a2d_cie_m24_t *m24 = &mcc->cie.m24_info;
        esp_aac_dec_cfg_t aac_cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
        aac_cfg.sample_rate = getSampleRate(mcc);
        aac_cfg.channel = getChannelCount(mcc);
        aac_cfg.bits_per_sample = ESP_AUDIO_BIT16;
        aac_cfg.no_adts_header = true;
        aac_cfg.aac_plus_enable = (m24->obj_type & (ESP_A2D_M24_CIE_OBJ_TYPE_4_HE_AAC |
                                                    ESP_A2D_M24_CIE_OBJ_TYPE_4_HE_AAC_V2)) != 0;

        esp_audio_dec_cfg_t dec_cfg = {
            .type = ESP_AUDIO_TYPE_AAC,
            .cfg = &aac_cfg,
            .cfg_sz = sizeof(esp_aac_dec_cfg_t),
        };

        esp_aac_dec_register();
        ret = esp_audio_dec_open(&dec_cfg, &_dec_hdl);
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(_DECODER_TAG, "esp_audio_dec_open (AAC) failed: %d", ret);
            return ret;
        }
        ESP_LOGI(_DECODER_TAG, "AAC decoder open: %" PRIu32 " Hz, %d ch, aac_plus=%d",
                 (uint32_t)aac_cfg.sample_rate, aac_cfg.channel, aac_cfg.aac_plus_enable);
    }

    _dec_opened = true;
    return ESP_AUDIO_ERR_OK;
}

void AudioDecoder::audio_decode_task()
{
    esp_a2d_audio_buff_t *audio_buf = nullptr;
    uint32_t max_out_size = 4096;
    uint8_t *out_buf = (uint8_t *)malloc((size_t)max_out_size);
    if (out_buf == NULL) {
        ESP_LOGE(_DECODER_TAG, "raw dec: out buffer alloc failed");
        return;
    }

    while (_running.load()) {
        audio_buf = _a2dp_raw_queue.take();
        if (!audio_buf) {
            continue;
        }

        esp_audio_dec_in_raw_t in_raw = {
            .buffer = audio_buf->data,
            .len = audio_buf->data_len,
        };
        esp_audio_dec_out_frame_t out_frame = {
            .buffer = out_buf,
            .len = max_out_size,
        };

        while (in_raw.len > 0) {
            esp_audio_err_t dec_ret = esp_audio_dec_process(_dec_hdl, &in_raw, &out_frame);
            if (dec_ret != ESP_AUDIO_ERR_OK && dec_ret != ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                ESP_LOGW(_DECODER_TAG, "esp_audio_dec_process failed: %d", dec_ret);
                break;
            }

            if (dec_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                uint8_t *new_dec_buff = (uint8_t *)realloc(out_frame.buffer, (size_t)max_out_size * 2u);
                if (new_dec_buff == NULL) {
                    break;
                }
                max_out_size *= 2;
                out_buf = new_dec_buff;
                out_frame.buffer = new_dec_buff;
                out_frame.len = max_out_size;
                continue;
            }

            if(_audio_i2s == nullptr) {
                ESP_LOGE(_DECODER_TAG, "AudioI2s handle is not registered");
                break;
            }
            _audio_i2s->sendToI2s(out_frame.buffer, out_frame.decoded_size);

            if(_ui_music_player != nullptr) {
                _ui_music_player->audioVisual(out_frame.buffer, out_frame.decoded_size);
            }

            in_raw.buffer += in_raw.consumed;
            in_raw.len -= in_raw.consumed;
        }
        esp_a2d_audio_buff_free(audio_buf);

    }
}

int AudioDecoder::getChannelCount(const esp_a2d_mcc_t *mcc)
{
    if (mcc->type == ESP_A2D_MCT_SBC) {
        if (mcc->cie.sbc_info.ch_mode & ESP_A2D_SBC_CIE_CH_MODE_MONO) {
            return 1;
        }
        return 2;
    }
    if (mcc->type == ESP_A2D_MCT_M24) {
        if (mcc->cie.m24_info.ch & ESP_A2D_M24_CIE_CH_1) {
            return 1;
        }
        return 2;
    }
    return 2;
}

int AudioDecoder::getSampleRate(const esp_a2d_mcc_t *mcc)
{
    if (mcc->type == ESP_A2D_MCT_SBC) {
        const esp_a2d_cie_sbc_t *sbc = &mcc->cie.sbc_info;
        if (sbc->samp_freq & ESP_A2D_SBC_CIE_SF_48K) {
            return 48000;
        }
        if (sbc->samp_freq & ESP_A2D_SBC_CIE_SF_44K) {
            return 44100;
        }
        if (sbc->samp_freq & ESP_A2D_SBC_CIE_SF_32K) {
            return 32000;
        }
        if (sbc->samp_freq & ESP_A2D_SBC_CIE_SF_16K) {
            return 16000;
        }
        return 44100;
    }
    if (mcc->type == ESP_A2D_MCT_M24) {
        const esp_a2d_cie_m24_t *m24 = &mcc->cie.m24_info;
        if (m24->samp_freq2 & ESP_A2D_M24_CIE_SF2_96K) {
            return 96000;
        }
        if (m24->samp_freq2 & ESP_A2D_M24_CIE_SF2_88K) {
            return 88200;
        }
        if (m24->samp_freq2 & ESP_A2D_M24_CIE_SF2_64K) {
            return 64000;
        }
        if (m24->samp_freq2 & ESP_A2D_M24_CIE_SF2_48K) {
            return 48000;
        }
        if (m24->samp_freq1 & ESP_A2D_M24_CIE_SF1_44K) {
            return 44100;
        }
        if (m24->samp_freq1 & ESP_A2D_M24_CIE_SF1_32K) {
            return 32000;
        }
        if (m24->samp_freq1 & ESP_A2D_M24_CIE_SF1_24K) {
            return 24000;
        }
        if (m24->samp_freq1 & ESP_A2D_M24_CIE_SF1_22K) {
            return 22050;
        }
        if (m24->samp_freq1 & ESP_A2D_M24_CIE_SF1_16K) {
            return 16000;
        }
        if (m24->samp_freq1 & ESP_A2D_M24_CIE_SF1_12K) {
            return 12000;
        }
        if (m24->samp_freq1 & ESP_A2D_M24_CIE_SF1_11K) {
            return 11025;
        }
        if (m24->samp_freq1 & ESP_A2D_M24_CIE_SF1_8K) {
            return 8000;
        }
        return 44100;
    }
    return 44100;
}