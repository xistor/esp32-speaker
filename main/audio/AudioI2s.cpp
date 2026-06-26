#include "AudioI2s.h"
#include "esp_pthread.h"
#include "freertos/idf_additions.h" 

AudioI2s::AudioI2s(const i2s_chan_config_t &chan_cfg, const i2s_std_config_t &std_cfg)
    : _chan_cfg(chan_cfg), _std_cfg(std_cfg)
{
    // other members are initialized via in-class defaults
}

AudioI2s::~AudioI2s()
{
    stop();
    if (_i2s_task_thread.joinable()) {
        _i2s_task_thread.join();
    }

    if (_i2s_write_semaphore) {
        vSemaphoreDelete(_i2s_write_semaphore);
        _i2s_write_semaphore = nullptr;
    }
    if (_ringbuf_i2s) {
        vRingbufferDelete(_ringbuf_i2s);
        _ringbuf_i2s = nullptr;
    }
}

void AudioI2s::configI2s(const i2s_chan_config_t& chan_cfg,
                        const i2s_std_config_t& std_cfg)
{
    _chan_cfg = chan_cfg;
    _std_cfg = std_cfg;
}

AudioI2sError AudioI2s::start()
{
    esp_err_t err;

    if (_running.load()) {
        ESP_LOGW(X_AUDIO_I2S_TAG, "start() called while already running");
        return AudioI2sError::NONE;
    }

    err = i2s_new_channel(&_chan_cfg, &_tx_chan, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(X_AUDIO_I2S_TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return AudioI2sError::NONE; // map properly if needed
    }

    err = i2s_channel_init_std_mode(_tx_chan, &_std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(X_AUDIO_I2S_TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        return AudioI2sError::NONE;
    }

    _ringbuffer_mode.store(RingbufferMode::PREFETCHING);

    _i2s_write_semaphore = xSemaphoreCreateBinary();
    if (_i2s_write_semaphore == nullptr) {
        ESP_LOGE(X_AUDIO_I2S_TAG, "%s, semaphore create failed", __func__);
        return AudioI2sError::SEMAPHORE_CREATE_FAILED;
    }

    _ringbuf_i2s = xRingbufferCreate(_ringbuf_size, RINGBUF_TYPE_BYTEBUF);
    if (_ringbuf_i2s == nullptr) {
        ESP_LOGE(X_AUDIO_I2S_TAG, "%s, ringbuffer create failed", __func__);
        vSemaphoreDelete(_i2s_write_semaphore);
        _i2s_write_semaphore = nullptr;
        return AudioI2sError::RINGBUFFER_CREATE_FAILED;
    }

    _running.store(true);


    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.thread_name = "i2STask";
    cfg.pin_to_core = 0; 
    cfg.prio = 15; 

    esp_pthread_set_cfg(&cfg);

    _i2s_task_thread = std::thread(&AudioI2s::i2sTask, this);

    ESP_LOGI(X_AUDIO_I2S_TAG, "AudioI2s::start success");
    return AudioI2sError::NONE;
}


void AudioI2s::stop()
{
    if (!_running.load()) {
        ESP_LOGW(X_AUDIO_I2S_TAG, "stop() called while not running");
        return;
    }

    _running.store(false);
    // wake up task in case it's waiting
    xSemaphoreGive(_i2s_write_semaphore);

    if (_i2s_task_thread.joinable()) {
        _i2s_task_thread.join();
    }

    if (_tx_chan) {
        ESP_ERROR_CHECK(i2s_channel_disable(_tx_chan));
        ESP_ERROR_CHECK(i2s_del_channel(_tx_chan));
        _tx_chan = nullptr;
    }

    if(_ringbuf_i2s) {
        vRingbufferDelete(_ringbuf_i2s);
        _ringbuf_i2s = nullptr;
    }

    ESP_LOGI(X_AUDIO_I2S_TAG, "AudioI2s::stop done");
}

void AudioI2s::enableI2s(void)
{
    ESP_ERROR_CHECK(i2s_channel_enable(_tx_chan));
}

void AudioI2s::reConfigI2s(const i2s_std_clk_config_t &clk_cfg, const i2s_std_slot_config_t &slot_cfg)
{
    i2s_channel_reconfig_std_clock(_tx_chan, &clk_cfg);
    i2s_channel_reconfig_std_slot(_tx_chan, &slot_cfg);

    ESP_LOGI(X_AUDIO_I2S_TAG, "AudioI2s::reconfigI2s done");
}

void AudioI2s::i2sTask()
{
    uint8_t *data = nullptr;
    size_t item_size = 0;
    // DMA buffer size trade‑off hint
    constexpr size_t item_size_upto = 240 * 6;
    size_t bytes_written = 0;

    while (_running.load()) {
        if (pdTRUE == xSemaphoreTake(_i2s_write_semaphore, portMAX_DELAY)) {
            while (_running.load()) {
                item_size = 0;
                data = static_cast<uint8_t *>(
                    xRingbufferReceiveUpTo(_ringbuf_i2s, &item_size, pdMS_TO_TICKS(20), item_size_upto));
                if (item_size == 0) {
                    ESP_LOGI(X_AUDIO_I2S_TAG, "ringbuffer underflowed! mode changed: PREFETCHING");
                    _ringbuffer_mode.store(RingbufferMode::PREFETCHING);
                    break;
                }
                i2s_channel_write(_tx_chan, data, item_size, &bytes_written, portMAX_DELAY);
                vRingbufferReturnItem(_ringbuf_i2s, data);
            }
        }
    }
}

size_t AudioI2s::sendToI2s(const uint8_t *data, size_t size)
{
    size_t item_size = 0;
    BaseType_t done = pdFALSE;

    if (_ringbuffer_mode == RingbufferMode::DROPPING) {
        ESP_LOGW(X_AUDIO_I2S_TAG, "ringbuffer is full, drop this packet!");
        vRingbufferGetInfo(_ringbuf_i2s, NULL, NULL, NULL, NULL, &item_size);
        if (item_size <= RINGBUF_PREFETCH_WATER_LEVEL) {
            ESP_LOGI(X_AUDIO_I2S_TAG, "ringbuffer data decreased! mode changed: PROCESSING");
            _ringbuffer_mode = RingbufferMode::PROCESSING;
        }
        return 0;
    }

    done = xRingbufferSend(_ringbuf_i2s, (void *)data, size, (TickType_t)0);

    if (!done) {
        ESP_LOGW(X_AUDIO_I2S_TAG, "ringbuffer overflowed, ready to decrease data! mode changed: DROPPING");
        _ringbuffer_mode = RingbufferMode::DROPPING;
    }

    if (_ringbuffer_mode == RingbufferMode::PREFETCHING) {
        vRingbufferGetInfo(_ringbuf_i2s, NULL, NULL, NULL, NULL, &item_size);
        if (item_size >= RINGBUF_PREFETCH_WATER_LEVEL) {
            ESP_LOGI(X_AUDIO_I2S_TAG, "ringbuffer data increased! mode changed: PROCESSING");
            _ringbuffer_mode = RingbufferMode::PROCESSING;
            if (pdFALSE == xSemaphoreGive(_i2s_write_semaphore)) {
                ESP_LOGE(X_AUDIO_I2S_TAG, "semaphore give failed");
            }
        }
    }

    return done ? size : 0;
}

void AudioI2s::clearI2sRingbuffer()
{
    i2s_channel_disable(_tx_chan);

    uint8_t *item = nullptr;
    size_t item_size = 0;
    if (_ringbuf_i2s) {

        while ((item = static_cast<uint8_t *>(xRingbufferReceive(_ringbuf_i2s, &item_size, 0))) != NULL) {
            vRingbufferReturnItem(_ringbuf_i2s, item);
        }

        ESP_LOGI(X_AUDIO_I2S_TAG, "I2S ringbuffer cleared");
    }

    i2s_channel_enable(_tx_chan);
}