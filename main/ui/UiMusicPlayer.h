#ifndef __UI_MUSIC_PLAYER_H__
#define __UI_MUSIC_PLAYER_H__

#include <functional>
#include <thread>
#include "lvgl.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"

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

    UiMusicPlayer();
    ~UiMusicPlayer();
    void create_ui();
    void setTitle(const char *title);
    void setArtist(const char *artist);
    void setPlaying(bool playing);
    void setCoverArt(const uint8_t *data);
    void setPlayTime(uint32_t play_time_ms);
    void setPlayPosition(uint32_t play_pos_ms);

    void regPlayCtrlCallback(PlayCtrlCallback cb);
    void audioVisual(const uint8_t *data, size_t size);
private:
    static UiMusicPlayer *s_instance;
    static constexpr const char *_MP_TAG = "UI_MPLAYER";
    static int16_t s_current_fft_bands[CONFIG_UI_SPECTRUM_BANDS_NUMS];

    static void play_ctrl_event_cb(lv_event_t * e);
    static void visual_switch_event_cb(lv_event_t * e);
    static void sp_timer_cb(lv_timer_t * timer);

    lv_obj_t *_album_art = nullptr;
    lv_obj_t *_title = nullptr;
    lv_obj_t *_artist = nullptr;
    lv_obj_t *_album_img = nullptr;
    lv_obj_t *_play_label = nullptr;
    lv_obj_t *_play_slider = nullptr;
    lv_obj_t *_sp_cont = nullptr;
    lv_obj_t *_band_objs[CONFIG_UI_SPECTRUM_BANDS_NUMS];

    lv_font_t *_font = nullptr;
    lv_anim_t _rotate_anim;
    lv_image_dsc_t _cover_img_dsc = {0};

    uint32_t _song_play_time_ms = 0;
    uint32_t _cur_play_pos_ms = 0;

    int _lock_timeout_ms = 500;
    PlayCtrlCallback _play_ctrl_cb = nullptr;

    std::atomic<bool> _fft_running{false};
    std::thread _fft_process_thread;
    float _fft_window[CONFIG_UI_FFT_SAMPLE_SIZE];
    float _fft_io_buffer[CONFIG_UI_FFT_SAMPLE_SIZE * 2];
    RingbufHandle_t _ringbuf_fft = nullptr; 
    size_t _fft_buf_size = 32 * 1024;
    uint8_t _band_widths[CONFIG_UI_SPECTRUM_BANDS_NUMS];

    SemaphoreHandle_t _fft_write_semaphore = nullptr;

    visualType _visual_type = visualType::SPECTRUM;

    void handlePlayCtrlEvent(lv_event_t * e);
    void fftProcessingTask();
    void calculateBandWidths();

};

#endif // __UI_MUSIC_PLAYER_H__