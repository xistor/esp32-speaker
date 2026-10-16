#ifndef __UI_MUSIC_PLAYER_H__
#define __UI_MUSIC_PLAYER_H__

#include <functional>
#include <thread>
#include "lvgl.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "esp_adc/adc_oneshot.h"
#include "UiSetting.h"

class UiMusicPlayer {
public:
    enum class playControlCmd : uint16_t {
        PLAY = 1,
        PAUSE = 2,
        PREVIOUS = 3,
        NEXT = 4,
    };

    enum class visualType : uint8_t {
        ALBUM_ART = 0,
        SPECTRUM = 1
    };

    typedef struct {
        playControlCmd cmd;
        uint8_t param[128];
    } play_ctrl_param_t;

    using PlayCtrlCallback = std::function<void(play_ctrl_param_t)>;


    static UiMusicPlayer& instance() {
        static UiMusicPlayer instance;
        return instance;
    }

    void init();
    void main_page();

    void setTitle(const char *title);
    void setArtist(const char *artist);
    void setPlaying(bool playing);
    void setCoverArt(const uint8_t *data);
    void setPlayTime(uint32_t play_time_ms);
    void setPlayPosition(uint32_t play_pos_ms);

    void regPlayCtrlCallback(PlayCtrlCallback cb);
    void audioVisual(const uint8_t *data, size_t size);

private:
    UiMusicPlayer();
    ~UiMusicPlayer();

    static constexpr const char *_MP_TAG = "UI_MPLAYER";
    static int16_t s_current_fft_bands[CONFIG_UI_SPECTRUM_BANDS_NUMS];

    static void play_ctrl_event_cb(lv_event_t * e);
    static void visual_switch_event_cb(lv_event_t * e);
    static void sp_timer_cb(lv_timer_t * timer);
    static void statusBarGestureCb(lv_event_t * e);

    lv_obj_t *_album_art = nullptr;
    lv_obj_t *_title = nullptr;
    lv_obj_t *_artist = nullptr;
    lv_obj_t *_album_img = nullptr;
    lv_obj_t *_play_label = nullptr;
    lv_obj_t *_play_slider = nullptr;
    lv_obj_t *_sp_cont = nullptr;
    lv_obj_t *_band_objs[CONFIG_UI_SPECTRUM_BANDS_NUMS];
    lv_obj_t *_battery_area = nullptr;
    lv_obj_t *_battery_bar = nullptr;
    lv_obj_t * _main_page = nullptr;


    lv_font_t *_font = nullptr;
    lv_anim_t _rotate_anim;
    lv_image_dsc_t _cover_img_dsc = {0};

    uint32_t _song_play_time_ms = 0;
    uint32_t _cur_play_pos_ms = 0;

    int _lock_timeout_ms = 500;
    PlayCtrlCallback _play_ctrl_cb = nullptr;

    std::atomic<bool> _fft_running{false};
    std::thread _fft_process_thread;

    static constexpr int FFT_SIZE = CONFIG_UI_FFT_SAMPLE_SIZE;
    static constexpr int FFT_NUM_BINS = FFT_SIZE / 2;
    // One analysis block: FFT_SIZE stereo int16 samples. This also caps how much
    // audio audioVisual() queues per call, so the ring buffer below always has
    // room for at least one whole item.
    static constexpr size_t FFT_BLOCK_BYTES = FFT_SIZE * sizeof(int16_t) * 2;
    // Four blocks of slack. The analyzer drains the ring buffer far faster than
    // audio arrives, so the buffer only has to absorb scheduling jitter - any
    // backlog here is audio we are about to throw away anyway.
    static constexpr size_t FFT_RINGBUF_SIZE = FFT_BLOCK_BYTES * 4;
    // The spectrum is redrawn every 33 ms, but audio at 44.1 kHz produces ~170
    // blocks/s. Computing every one of them throws away most of the work, so the
    // analyzer runs at 50 Hz and always keeps the newest block instead.
    static constexpr int64_t FFT_FRAME_INTERVAL_US = 20000;

    float *_fft_window = nullptr;   // Hann window, FFT_SIZE floats
    float *_fft_io_buffer = nullptr; // complex FFT input/output, FFT_SIZE * 2 floats
    int16_t *_fft_block = nullptr;   // accumulating stereo PCM, FFT_SIZE * 2 samples
    RingbufHandle_t _ringbuf_fft = nullptr;
    // Last bin index owned by each band. Ends tile bins [1, FFT_NUM_BINS)
    // exactly, so the band pass is a single walk with no per-bin bounds test.
    uint16_t _band_bin_end[CONFIG_UI_SPECTRUM_BANDS_NUMS];

    visualType _visual_type = visualType::SPECTRUM;

    adc_oneshot_unit_handle_t _adc1_handle;
    std::thread _bat_volt_read_thread;

    void handlePlayCtrlEvent(lv_event_t * e);
    void fftProcessingTask();
    void processFftBlock();
    void calculateBandEnds();

    static void applyBandEnergy(int band, float energy_sum, int bins);

    void initBatteryAdc();
    float getRealBatteryVoltage();
    void batteryVoltageReadTask();
    float getBatteryPercentage(float voltage);
    
    void handleVisualSwitchEvent(lv_event_t * e);

    UiSetting &_ui_setting = UiSetting::instance();
};

#endif // __UI_MUSIC_PLAYER_H__