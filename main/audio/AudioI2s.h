#ifndef __AUDIO_I2S_H__
#define __AUDIO_I2S_H__

#include <thread>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define X_AUDIO_I2S_TAG       "AUDIO_I2S"

static constexpr size_t RINGBUF_PREFETCH_WATER_LEVEL = 20 * 1024;

enum class RingbufferMode {
    PROCESSING,    /* ringbuffer is buffering incoming audio data, I2S is working */
    PREFETCHING,   /* ringbuffer is buffering incoming audio data, I2S is waiting */
    DROPPING       /* ringbuffer is not buffering (dropping) incoming audio data, I2S is working */
};

enum class AudioI2sError {
    NONE = 0,
    SEMAPHORE_CREATE_FAILED,
    RINGBUFFER_CREATE_FAILED,
    THREAD_START_FAILED,
};

class AudioI2s
{
private:
    i2s_chan_handle_t _tx_chan = nullptr;

    i2s_chan_config_t _chan_cfg{};
    i2s_std_config_t _std_cfg{};

    std::thread _i2s_task_thread;
    std::atomic<bool> _running{false};

    SemaphoreHandle_t _i2s_write_semaphore = nullptr;

    RingbufHandle_t _ringbuf_i2s = nullptr;     /* handle of ringbuffer for I2S */
    std::atomic<RingbufferMode> _ringbuffer_mode{RingbufferMode::PREFETCHING};
    size_t _ringbuf_size = 64 * 1024;

    /* private func */
    void i2sTask();

public:
    explicit AudioI2s(const i2s_chan_config_t& chan_cfg = {},
                      const i2s_std_config_t& std_cfg = {});
    ~AudioI2s();

    // Disable copy operations
    AudioI2s(const AudioI2s&) = delete;
    AudioI2s& operator=(const AudioI2s&) = delete;

    AudioI2sError start();
    void stop();

    void enableI2s();
    void configI2s(const i2s_chan_config_t &chan_cfg, const i2s_std_config_t &std_cfg);
    void reConfigI2s(const i2s_std_clk_config_t &clk_cfg, const i2s_std_slot_config_t &slot_cfg);

    size_t sendToI2s(const uint8_t *data, size_t size);
};



#endif /* __AUDIO_I2S_H__ */