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

// The AirPlay pipeline already buffers ~1.5 s in PSRAM and writes PCM at a
// paced rate. The I2S ring covers scheduling jitter between the playback
// thread and the I2S task; UI rendering and cover-art decoding can stall the
// producer for >100 ms, so keep a deep ring to avoid feeding silence in the
// middle of a track (that silence is audible as a click).
static constexpr size_t RINGBUF_PREFETCH_WATER_LEVEL = 8 * 1024;
static constexpr size_t RINGBUF_RESUME_WATER_LEVEL = 64 * 1024;
static constexpr size_t RINGBUF_HIGHEST_WATER_LEVEL = 96 * 1024;

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
    uint8_t* _ringbuf_storage = nullptr;        /* PSRAM-backed ring storage */
    StaticRingbuffer_t* _ringbuf_control = nullptr;
    std::atomic<RingbufferMode> _ringbuffer_mode{RingbufferMode::PREFETCHING};
    std::atomic<bool> _prefetch_required{true};
    std::atomic<uint32_t> _i2s_underruns{0};
    std::atomic<int32_t> _output_volume_q15{32768};
    size_t _ringbuf_size = RINGBUF_HIGHEST_WATER_LEVEL;

    // Last stereo frame written to I2S, used to fade out instead of cutting
    // straight to silence when the ring runs dry. The fade back in when data
    // resumes avoids a second click.
    static constexpr size_t FADE_FRAMES = 64;
    static constexpr size_t FADE_SAMPLES = FADE_FRAMES * 2;
    int16_t _last_output[2] = {0, 0};
    size_t _fade_in_remaining = 0;

    /* private func */
    void i2sTask();
    void destroyRingbuffer();
    void applyOutputVolume(uint8_t *data, size_t size);
    void applyFadeIn(int16_t *samples, size_t count);
    void rememberLastOutput(const int16_t *samples, size_t count);

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
    // Linear PCM16 gain, 0..32768 (Q15). 32768 leaves the stream untouched.
    // Applied while feeding I2S so a volume change takes effect immediately
    // even if audio is already buffered in the ring.
    void setOutputVolumeQ15(int32_t gain) { _output_volume_q15.store(gain); }
    void clearI2sRingbuffer();
    uint32_t underrunCount() const { return _i2s_underruns.load(); }
    // Bytes currently queued in the I2S ring. The playback thread uses this
    // to keep a cushion so scheduler jitter does not turn into silence.
    size_t bufferedBytes();
};



#endif /* __AUDIO_I2S_H__ */