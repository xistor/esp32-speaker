#include "AudioI2s.h"
#include "esp_heap_caps.h"
#include "esp_pthread.h"
#include "esp_timer.h"
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
    destroyRingbuffer();
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
    ESP_ERROR_CHECK(i2s_channel_enable(_tx_chan));

    _ringbuffer_mode.store(RingbufferMode::PREFETCHING);
    _prefetch_required.store(true);

    _last_output[0] = 0;
    _last_output[1] = 0;
    _fade_in_remaining = 0;

    _i2s_write_semaphore = xSemaphoreCreateBinary();
    if (_i2s_write_semaphore == nullptr) {
        ESP_LOGE(X_AUDIO_I2S_TAG, "%s, semaphore create failed", __func__);
        return AudioI2sError::SEMAPHORE_CREATE_FAILED;
    }

    // Keep the ringbuffer control block (and its spinlock) in internal RAM,
    // but put the 48 KB of sample storage in PSRAM. The audio pipeline needs
    // the internal heap for Wi-Fi/LWIP and task stacks.
    _ringbuf_control = static_cast<StaticRingbuffer_t*>(
        heap_caps_malloc(sizeof(StaticRingbuffer_t),
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    _ringbuf_storage = static_cast<uint8_t*>(
        heap_caps_malloc(_ringbuf_size,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (_ringbuf_control != nullptr && _ringbuf_storage != nullptr) {
        _ringbuf_i2s = xRingbufferCreateStatic(
            _ringbuf_size, RINGBUF_TYPE_BYTEBUF, _ringbuf_storage,
            _ringbuf_control);
    }
    if (_ringbuf_i2s == nullptr) {
        ESP_LOGE(X_AUDIO_I2S_TAG, "%s, ringbuffer create failed", __func__);
        heap_caps_free(_ringbuf_storage);
        heap_caps_free(_ringbuf_control);
        _ringbuf_storage = nullptr;
        _ringbuf_control = nullptr;
        vSemaphoreDelete(_i2s_write_semaphore);
        _i2s_write_semaphore = nullptr;
        return AudioI2sError::RINGBUFFER_CREATE_FAILED;
    }

    _running.store(true);


    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.thread_name = "i2STask";
    // Keep the DMA fed even while the UI renderer is busy on core 0. Core 1
    // only hosts the AirPlay worker threads, which are all preemptible.
    cfg.pin_to_core = 1;
    cfg.prio = 20;
    cfg.stack_size = 1024 * 3;

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
    } else {
        ESP_LOGW(X_AUDIO_I2S_TAG, "i2s_task_thread not joinable");
    }

    if (_tx_chan) {
        ESP_ERROR_CHECK(i2s_channel_disable(_tx_chan));
        ESP_ERROR_CHECK(i2s_del_channel(_tx_chan));
        _tx_chan = nullptr;
    }

    destroyRingbuffer();

    ESP_LOGI(X_AUDIO_I2S_TAG, "AudioI2s::stop done");
}

void AudioI2s::destroyRingbuffer()
{
    if (_ringbuf_i2s != nullptr) {
        vRingbufferDelete(_ringbuf_i2s);
        _ringbuf_i2s = nullptr;
    }
    if (_ringbuf_storage != nullptr) {
        heap_caps_free(_ringbuf_storage);
        _ringbuf_storage = nullptr;
    }
    if (_ringbuf_control != nullptr) {
        heap_caps_free(_ringbuf_control);
        _ringbuf_control = nullptr;
    }
}

void AudioI2s::enableI2s(void)
{
    ESP_ERROR_CHECK(i2s_channel_enable(_tx_chan));
}

void AudioI2s::reConfigI2s(const i2s_std_clk_config_t &clk_cfg, const i2s_std_slot_config_t &slot_cfg)
{
    ESP_ERROR_CHECK(i2s_channel_disable(_tx_chan));

    i2s_channel_reconfig_std_clock(_tx_chan, &clk_cfg);
    i2s_channel_reconfig_std_slot(_tx_chan, &slot_cfg);

    ESP_ERROR_CHECK(i2s_channel_enable(_tx_chan));

    ESP_LOGI(X_AUDIO_I2S_TAG, "AudioI2s::reconfigI2s done");
}

void AudioI2s::i2sTask()
{
    uint8_t *data = nullptr;
    size_t item_size = 0;
    // DMA buffer size trade‑off hint
    constexpr size_t item_size_upto = 240 * 6;
    constexpr size_t silence_chunk = 256;
    constexpr size_t silence_target = 1024; // ~5.8 ms at 44.1 kHz stereo
    uint8_t silence[silence_chunk] = {};
    size_t bytes_written = 0;

    while (_running.load()) {
        if (_prefetch_required.load()) {
            if (xSemaphoreTake(_i2s_write_semaphore, pdMS_TO_TICKS(100)) !=
                pdTRUE) {
                continue;
            }
            _prefetch_required.store(false);
        }

        item_size = 0;
        data = static_cast<uint8_t *>(
            xRingbufferReceiveUpTo(_ringbuf_i2s, &item_size,
                                   pdMS_TO_TICKS(10), item_size_upto));
        if (item_size == 0) {
            if (!_running.load()) {
                break;
            }
            // Keep the DMA running. If the channel is configured with
            // auto-clear the peripheral already sends zeros whenever no data
            // is queued; otherwise explicitly feed it silence so it does not
            // repeat stale samples.
            if (!_prefetch_required.load()) {
                _i2s_underruns.fetch_add(1, std::memory_order_relaxed);

                // Ramp the last frame down to silence; stepping straight to
                // zero is what turns a short underrun into an audible click.
                if (_fade_in_remaining == 0 &&
                    (_last_output[0] != 0 || _last_output[1] != 0)) {
                    int16_t fade[FADE_SAMPLES];
                    constexpr int32_t denom =
                        static_cast<int32_t>(FADE_FRAMES - 1);
                    for (size_t i = 0; i < FADE_FRAMES; ++i) {
                        const int32_t scale =
                            denom - static_cast<int32_t>(i);
                        fade[i * 2] = static_cast<int16_t>(
                            (static_cast<int32_t>(_last_output[0]) * scale) /
                            denom);
                        fade[i * 2 + 1] = static_cast<int16_t>(
                            (static_cast<int32_t>(_last_output[1]) * scale) /
                            denom);
                    }
                    i2s_channel_write(_tx_chan, fade, sizeof(fade),
                                      &bytes_written, pdMS_TO_TICKS(100));
                    _last_output[0] = 0;
                    _last_output[1] = 0;
                    _fade_in_remaining = FADE_SAMPLES;
                }

                if (!_chan_cfg.auto_clear) {
                    for (size_t written = 0; written < silence_target;
                         written += sizeof(silence)) {
                        i2s_channel_write(_tx_chan, silence, sizeof(silence),
                                          &bytes_written, pdMS_TO_TICKS(100));
                    }
                }
            }
            continue;
        }

        applyOutputVolume(data, item_size);
        auto *samples = reinterpret_cast<int16_t *>(data);
        const size_t count = item_size / sizeof(int16_t);
        applyFadeIn(samples, count);
        rememberLastOutput(samples, count);
        i2s_channel_write(_tx_chan, data, item_size, &bytes_written,
                          portMAX_DELAY);
        vRingbufferReturnItem(_ringbuf_i2s, data);
    }
}

void AudioI2s::applyOutputVolume(uint8_t *data, size_t size)
{
    const int32_t gain = _output_volume_q15.load(std::memory_order_relaxed);
    if (gain >= 32768) {
        return;
    }

    const size_t count = size / sizeof(int16_t);
    if (count == 0) {
        return;
    }

    if (gain <= 0) {
        memset(data, 0, count * sizeof(int16_t));
        return;
    }

    auto *samples = reinterpret_cast<int16_t *>(data);
    for (size_t i = 0; i < count; ++i) {
        samples[i] = static_cast<int16_t>(
            (static_cast<int32_t>(samples[i]) * gain) >> 15);
    }
}

void AudioI2s::applyFadeIn(int16_t *samples, size_t count)
{
    if (_fade_in_remaining == 0 || count == 0) {
        return;
    }

    const size_t fade = count < _fade_in_remaining ? count : _fade_in_remaining;
    for (size_t i = 0; i < fade; ++i) {
        samples[i] = static_cast<int16_t>(
            (static_cast<int32_t>(samples[i]) * static_cast<int32_t>(i)) /
            static_cast<int32_t>(FADE_SAMPLES));
    }
    _fade_in_remaining -= fade;
}

void AudioI2s::rememberLastOutput(const int16_t *samples, size_t count)
{
    if (count == 0) {
        return;
    }
    _last_output[0] = samples[count > 1 ? count - 2 : count - 1];
    _last_output[1] = samples[count - 1];
}

size_t AudioI2s::bufferedBytes()
{
    if (_ringbuf_i2s == nullptr) {
        return 0;
    }
    size_t buffered = 0;
    vRingbufferGetInfo(_ringbuf_i2s, nullptr, nullptr, nullptr, nullptr,
                       &buffered);
    return buffered;
}

size_t AudioI2s::sendToI2s(const uint8_t *data, size_t size)
{
    size_t item_size = 0;
    BaseType_t done = pdFALSE;
    static int fail_count = 0;

    if (_ringbuffer_mode == RingbufferMode::DROPPING) {
        // One line per dropped frame would cost several milliseconds of UART
        // time each and keep the producer in the drop path even longer.
        static int64_t last_drop_log_us = 0;
        int64_t nowUs = esp_timer_get_time();
        if (nowUs - last_drop_log_us > 1000000) {
            last_drop_log_us = nowUs;
            ESP_LOGW(X_AUDIO_I2S_TAG, "ringbuffer is full, drop this packet!");
        }
        fail_count++;
        vRingbufferGetInfo(_ringbuf_i2s, NULL, NULL, NULL, NULL, &item_size);
        if (item_size <= RINGBUF_RESUME_WATER_LEVEL) {
            ESP_LOGI(X_AUDIO_I2S_TAG, "ringbuffer data decreased! mode changed: PROCESSING");
            _ringbuffer_mode = RingbufferMode::PROCESSING;
            _prefetch_required.store(false);
            if (_i2s_write_semaphore != nullptr) {
                xSemaphoreGive(_i2s_write_semaphore);
            }
            fail_count = 0;
        }

        if (fail_count > 32) {
            ESP_LOGE(X_AUDIO_I2S_TAG, "ringbuffer is full, drop too many packets! reset");
            clearI2sRingbuffer();
            _ringbuffer_mode = RingbufferMode::PREFETCHING;
            fail_count = 0;
        }
        return 0;
    }

    // Give the I2S task a chance to drain instead of dropping the new
    // frame immediately. A full ring at a track boundary used to flip into
    // DROPPING and discard ~100 ms of the new track.
    done = xRingbufferSend(_ringbuf_i2s, (void *)data, size,
                           pdMS_TO_TICKS(20));

    if (!done) {
        ESP_LOGW(X_AUDIO_I2S_TAG, "ringbuffer overflowed, ready to decrease data! mode changed: DROPPING");
        _ringbuffer_mode = RingbufferMode::DROPPING;
    }

    if (_ringbuffer_mode == RingbufferMode::PREFETCHING) {
        vRingbufferGetInfo(_ringbuf_i2s, NULL, NULL, NULL, NULL, &item_size);
        if (item_size >= RINGBUF_PREFETCH_WATER_LEVEL) {
            ESP_LOGI(X_AUDIO_I2S_TAG, "ringbuffer data increased! mode changed: PROCESSING");
            _ringbuffer_mode = RingbufferMode::PROCESSING;
            _prefetch_required.store(false);
            if (_i2s_write_semaphore != nullptr &&
                pdFALSE == xSemaphoreGive(_i2s_write_semaphore)) {
                ESP_LOGE(X_AUDIO_I2S_TAG, "semaphore give failed");
            }
        }
    }

    return done ? size : 0;
}

void AudioI2s::clearI2sRingbuffer()
{
    if (_ringbuf_i2s == nullptr) {
        ESP_LOGW(X_AUDIO_I2S_TAG,
                 "clearI2sRingbuffer called but ringbuffer is not initialized");
        return;
    }

    // Do not disable the I2S channel here. This function is called from the
    // RTSP thread while the I2S task may be blocked in i2s_channel_write();
    // disabling the channel from another task can deadlock the RTSP thread.
    // Draining the ring buffer is enough; the I2S task will underflow and
    // switch back to PREFETCHING on its own.
    uint8_t* item = nullptr;
    size_t itemSize = 0;
    while ((item = static_cast<uint8_t*>(
                xRingbufferReceive(_ringbuf_i2s, &itemSize, 0))) != nullptr) {
        vRingbufferReturnItem(_ringbuf_i2s, item);
    }
    _ringbuffer_mode.store(RingbufferMode::PREFETCHING);
    _prefetch_required.store(true);
    if (_i2s_write_semaphore != nullptr) {
        // Drop a stale wake-up so the next prefetch really waits for data.
        xSemaphoreTake(_i2s_write_semaphore, 0);
    }
    ESP_LOGI(X_AUDIO_I2S_TAG, "I2S ringbuffer cleared");
}
