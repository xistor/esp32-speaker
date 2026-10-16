#include "esp_err.h"
#include "esp_log.h"
#include "UiMusicPlayer.h"
#include "LvglManager.h"
#include <cmath>
#include <cstring>
#include "esp_dsp.h"
#include "esp_pthread.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

    LV_IMAGE_DECLARE(default_albumart);

#ifdef __cplusplus
}
#endif

extern "C" {
    LV_FONT_DECLARE(Noto_14);
    LV_FONT_DECLARE(awesome_14);
}

int16_t UiMusicPlayer::s_current_fft_bands[CONFIG_UI_SPECTRUM_BANDS_NUMS];


UiMusicPlayer::UiMusicPlayer()
{

    _ringbuf_fft = xRingbufferCreate(FFT_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (_ringbuf_fft == nullptr) {
        ESP_LOGE(_MP_TAG, "Failed to create ring buffer for FFT data");
        return;
    }

    // The analyzer walks these buffers several times per frame. Handing them to
    // plain new() lets them land in PSRAM under memory pressure, where every
    // pass costs cache misses over SPI - pin them to internal DRAM instead.
    _fft_window = static_cast<float *>(
        heap_caps_calloc(FFT_SIZE, sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    _fft_io_buffer = static_cast<float *>(
        heap_caps_calloc(FFT_SIZE * 2, sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    _fft_block = static_cast<int16_t *>(
        heap_caps_calloc(FFT_SIZE * 2, sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    if (_fft_window == nullptr || _fft_io_buffer == nullptr || _fft_block == nullptr) {
        ESP_LOGE(_MP_TAG, "Failed to allocate FFT working buffers");
    }
}

UiMusicPlayer::~UiMusicPlayer()
{

    _fft_running.store(false);

    // fftProcessingTask wakes on its ring buffer timeout, so the join is bounded.
    if (_fft_process_thread.joinable()) {
        _fft_process_thread.join();
    }

    if(_ringbuf_fft) {
        vRingbufferDelete(_ringbuf_fft);
        _ringbuf_fft = nullptr;
    }

    if(_fft_window) {
        heap_caps_free(_fft_window);
        _fft_window = nullptr;
    }

    if(_fft_io_buffer) {
        heap_caps_free(_fft_io_buffer);
        _fft_io_buffer = nullptr;
    }

    if(_fft_block) {
        heap_caps_free(_fft_block);
        _fft_block = nullptr;
    }

}

void UiMusicPlayer::sp_timer_cb(lv_timer_t * timer) {

    for(int i = 0; i < CONFIG_UI_SPECTRUM_BANDS_NUMS; i++) {
            lv_obj_set_height(UiMusicPlayer::instance()._band_objs[i], (int32_t)UiMusicPlayer::instance().s_current_fft_bands[i]);
    }

}

void UiMusicPlayer::statusBarGestureCb(lv_event_t * e)
{
    ESP_LOGI(_MP_TAG, "Status bar gesture detected");

    lv_indev_t * indev = lv_indev_active();
    if(indev == nullptr) return;

    // Only react to a swipe (drag) downwards started on the status bar
    if(lv_indev_get_gesture_dir(indev) == LV_DIR_BOTTOM) {
        ESP_LOGI(_MP_TAG, "Swipe down on status bar, opening settings page");
        UiMusicPlayer::instance()._ui_setting.createSettingPage();
    }
}

void UiMusicPlayer::init()
{

    main_page();

    // The LVGL pool is a fixed static array sized by CONFIG_LV_MEM_SIZE_KILOBYTES, and
    // LVGL spins on a failed allocation rather than reporting it. Log the real high-water
    // mark so the pool can be sized from measurement instead of guesswork.
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(_MP_TAG, "LVGL pool: total=%u max_used=%u free=%u biggest_free=%u frag=%u%%",
             (unsigned)mon.total_size, (unsigned)mon.max_used, (unsigned)mon.free_size,
             (unsigned)mon.free_biggest_size, mon.frag_pct);
}

void UiMusicPlayer::main_page()
{

    if(LvglManager::lvgl_lock(_lock_timeout_ms) == false) {
        ESP_LOGE(_MP_TAG, "Failed to acquire LVGL lock to create UI");
        return;
    }

    // 1. Background Container
    _main_page = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_main_page, CONFIG_LCD_H_RES, CONFIG_LCD_V_RES);
    lv_obj_set_style_bg_color(_main_page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(_main_page, 0, 0);
    lv_obj_set_style_radius(_main_page, 0, 0);
    lv_obj_remove_flag(_main_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(_main_page);
    
    // 1.0 Top status bar gesture zone - swipe down to open the settings page.
    //     Transparent, placed underneath the battery/setting icons so they stay tappable.
    lv_obj_t * status_bar = lv_obj_create(_main_page);
    lv_obj_remove_style_all(status_bar);
    lv_obj_set_size(status_bar, CONFIG_LCD_H_RES, 30);
    lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(status_bar, statusBarGestureCb, LV_EVENT_GESTURE, NULL);

    // 1.1 battery
    _battery_area = lv_obj_create(_main_page);
    lv_obj_remove_style_all(_battery_area); 
    lv_obj_set_size(_battery_area, 25, 20);
    lv_obj_align(_battery_area, LV_ALIGN_TOP_LEFT, 12, 5); 

    // _battery_text = lv_label_create(_battery_area);
    // lv_label_set_text(_battery_text, "--%");
    // lv_obj_set_style_text_font(_battery_text, &awesome_14, 0); 
    // lv_obj_align(_battery_text, LV_ALIGN_LEFT_MID, 0, 0); 

    lv_obj_t* battery_shell = lv_obj_create(_battery_area);
    lv_obj_set_size(battery_shell, 20, 10);
    lv_obj_align(battery_shell, LV_ALIGN_RIGHT_MID, -5, 0); 
    lv_obj_set_style_border_width(battery_shell, 1, 0); 
    lv_obj_set_style_border_color(battery_shell, lv_color_white(), 0); 
    lv_obj_set_style_radius(battery_shell, 2, 0); 
    lv_obj_set_style_bg_opa(battery_shell, LV_OPA_TRANSP, 0); 
    lv_obj_remove_flag(battery_shell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(battery_shell, 0, 0);
    lv_obj_set_style_clip_corner(battery_shell, 0, 0);

    lv_obj_t* battery_tip = lv_obj_create(_battery_area);
    lv_obj_remove_style_all(battery_tip); 
    lv_obj_set_size(battery_tip, 2, 4);
    lv_obj_align_to(battery_tip, battery_shell, LV_ALIGN_OUT_RIGHT_MID, 0, 0); 
    lv_obj_set_style_bg_color(battery_tip, lv_color_white(), 0); 
    lv_obj_set_style_bg_opa(battery_tip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(battery_tip, 1, 0); 

    _battery_bar = lv_obj_create(battery_shell);
    lv_obj_remove_style_all(_battery_bar);

    lv_obj_set_size(_battery_bar, 10, 6); 
    lv_obj_align(_battery_bar, LV_ALIGN_LEFT_MID, 1, 0);

    lv_obj_set_style_bg_color(_battery_bar, lv_palette_main(LV_PALETTE_GREEN), 0); 
    lv_obj_set_style_bg_opa(_battery_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(_battery_bar, 1, 0);


    // 2. Album Art - Now at the TOP
     _album_art = lv_image_create(_main_page);
    lv_obj_set_size(_album_art, 130, 130);
    lv_obj_set_style_bg_color(_album_art, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_border_width(_album_art, 0, 0);
    lv_obj_set_style_pad_all(_album_art, 0, 0);
    lv_obj_align(_album_art, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_clear_flag(_album_art, LV_OBJ_FLAG_SCROLLABLE);
    lv_image_set_src(_album_art, &default_albumart);
    uint32_t scale_factor = (130 * LV_SCALE_NONE) / 200; // Result is 166 (approx 65%)

    lv_image_set_scale(_album_art, scale_factor);
    lv_image_set_pivot(_album_art, 100, 100); // Center of the original 200x200 image

    lv_obj_add_flag(_album_art, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_album_art, visual_switch_event_cb, LV_EVENT_CLICKED, NULL);

    // 2.5 Spectrum Visualizer
    _sp_cont = lv_obj_create(_main_page);
    int _sp_cont_width = CONFIG_LCD_H_RES - 40;
    lv_obj_set_size(_sp_cont, _sp_cont_width, 120); 
    lv_obj_align(_sp_cont, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_set_style_pad_all(_sp_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(_sp_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(_sp_cont, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_sp_cont, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_set_layout(_sp_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_sp_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(_sp_cont, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    lv_obj_remove_flag(_sp_cont, LV_OBJ_FLAG_SCROLLABLE);

    int bar_width = _sp_cont_width / CONFIG_UI_SPECTRUM_BANDS_NUMS;

    for(int i = 0; i < CONFIG_UI_SPECTRUM_BANDS_NUMS; i++) {
        _band_objs[i] = lv_obj_create(_sp_cont);

        lv_obj_set_style_pad_all(_band_objs[i], 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(_band_objs[i], 0, LV_PART_MAIN);
        
        lv_obj_set_size(_band_objs[i], bar_width, 4); 
        
        lv_obj_set_style_radius(_band_objs[i], 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(_band_objs[i], lv_color_hex(0x00A2FF), LV_PART_MAIN); 
        
        lv_obj_remove_flag(_band_objs[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(_band_objs[i], LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_add_flag(_sp_cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_sp_cont, visual_switch_event_cb, LV_EVENT_CLICKED, NULL);

    lv_timer_create(sp_timer_cb, 33, NULL);
    lv_obj_add_flag(_sp_cont, LV_OBJ_FLAG_HIDDEN); 

    // 3. Song Title - Positioned BELOW Album Art
    _title = lv_label_create(_main_page);
    lv_label_set_text(_title, "No Song Playing");
    lv_obj_set_width(_title, CONFIG_LCD_H_RES);
    lv_obj_set_style_text_color(_title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_long_mode(_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_title, &awesome_14, 0);
    lv_obj_align_to(_title, _sp_cont, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    // 4. Artist Name - Positioned BELOW Song Title
    _artist = lv_label_create(_main_page);
    lv_label_set_text(_artist, "No Artist");
    lv_obj_set_width(_artist, CONFIG_LCD_H_RES);
    lv_obj_set_height(_artist, 20);
    lv_obj_set_style_text_color(_artist, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_align(_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(_artist, &awesome_14, 0);
    lv_obj_align_to(_artist, _title, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

    // 5. Progress Slider
    _play_slider = lv_slider_create(_main_page);
    lv_obj_set_width(_play_slider, 200);
    lv_obj_set_height(_play_slider, 6);
    lv_obj_align(_play_slider, LV_ALIGN_BOTTOM_MID, 0, -80);
    lv_obj_set_style_bg_color(_play_slider, lv_color_hex(0x1DB954), LV_PART_INDICATOR);

    // 6. contrl button
    lv_obj_t * btn_play = lv_btn_create(_main_page);
    lv_obj_set_size(btn_play, 50, 50);
    lv_obj_set_style_radius(btn_play, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn_play, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(btn_play, LV_ALIGN_BOTTOM_MID, 0, -20);

    _play_label = lv_label_create(btn_play);
    lv_label_set_text(_play_label, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(_play_label, lv_color_hex(0x000000), 0);
    lv_obj_center(_play_label);
    lv_obj_add_event_cb(btn_play, play_ctrl_event_cb, LV_EVENT_CLICKED, (void *)1);

    // --- prev ---
    lv_obj_t * btn_prev = lv_btn_create(_main_page);
    lv_obj_set_size(btn_prev, 60, 60);
    lv_obj_align_to(btn_prev, btn_play, LV_ALIGN_OUT_LEFT_MID, -15, 0);

    lv_obj_set_style_bg_opa(btn_prev, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(btn_prev, 0, 0);
    lv_obj_set_style_border_width(btn_prev, 0, 0);

    lv_obj_t * label_prev = lv_label_create(btn_prev);
    lv_label_set_text(label_prev, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(label_prev, &awesome_14, 0);
    lv_obj_set_style_text_color(label_prev, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label_prev);
    lv_obj_add_event_cb(btn_prev, play_ctrl_event_cb, LV_EVENT_CLICKED, (void *)2);

    // --- next ---
    lv_obj_t * btn_next = lv_btn_create(_main_page);
    lv_obj_set_size(btn_next, 60, 60);
    lv_obj_align_to(btn_next, btn_play, LV_ALIGN_OUT_RIGHT_MID, 15, 0);

    lv_obj_set_style_bg_opa(btn_next, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(btn_next, 0, 0);
    lv_obj_set_style_border_width(btn_next, 0, 0);

    lv_obj_t * label_next = lv_label_create(btn_next);
    lv_label_set_text(label_next, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(label_next, &awesome_14, 0);
    lv_obj_set_style_text_color(label_next, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label_next);
    lv_obj_add_event_cb(btn_next, play_ctrl_event_cb, LV_EVENT_CLICKED, (void *)3);

    LvglManager::lvgl_unlock();

    initBatteryAdc();

    esp_pthread_cfg_t bat_cfg = esp_pthread_get_default_config();
    bat_cfg.thread_name = "batMonTask";
    bat_cfg.pin_to_core = tskNO_AFFINITY;
    bat_cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    esp_pthread_set_cfg(&bat_cfg);

    _bat_volt_read_thread = std::thread(&UiMusicPlayer::batteryVoltageReadTask, this);

    _fft_running.store(true);


    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 1024 * 2 ;
    cfg.thread_name = "fftProcTask";
    cfg.pin_to_core = 1;
    cfg.stack_alloc_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    esp_pthread_set_cfg(&cfg);

    _fft_process_thread = std::thread(&UiMusicPlayer::fftProcessingTask, this);
    _visual_type = visualType::ALBUM_ART;

}

void UiMusicPlayer::setTitle(const char *title)
{
    if(LvglManager::lvgl_lock(_lock_timeout_ms) == false) {
        ESP_LOGE(_MP_TAG, "Failed to acquire LVGL lock to set title");
        return;
    }

    if(_title) {
        lv_label_set_text(_title, title);
    }
    LvglManager::lvgl_unlock();
}

void UiMusicPlayer::setArtist(const char *artist)
{
    if(LvglManager::lvgl_lock(_lock_timeout_ms) == false) {
        ESP_LOGE(_MP_TAG, "Failed to acquire LVGL lock to set artist");
        return;
    }

    if(_artist) {
        lv_label_set_text(_artist, artist);
    }
    LvglManager::lvgl_unlock();

}

void UiMusicPlayer::setPlaying(bool playing)
{
    if(LvglManager::lvgl_lock(_lock_timeout_ms) == false) {
        ESP_LOGE(_MP_TAG, "Failed to acquire LVGL lock to set playing state");
        return;
    }

    if(playing) {
        lv_label_set_text(_play_label, LV_SYMBOL_PAUSE);
    } else {
        lv_label_set_text(_play_label, LV_SYMBOL_PLAY);
    }

    LvglManager::lvgl_unlock();
}

void UiMusicPlayer::setCoverArt(const uint8_t *p_data)
{
    if(LvglManager::lvgl_lock(_lock_timeout_ms) == false) {
        ESP_LOGE(_MP_TAG, "Failed to acquire LVGL lock to set cover art");
        return;
    }
    _cover_img_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    _cover_img_dsc.header.cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
    _cover_img_dsc.header.w = 200;
    _cover_img_dsc.header.h = 200;
    _cover_img_dsc.data_size = 200 * 200 * sizeof(uint16_t);
    _cover_img_dsc.data = p_data;
        
    if(_album_art) {
        lv_image_set_src(_album_art, &_cover_img_dsc);
        lv_obj_invalidate(_album_art);
    } else {
        ESP_LOGW(_MP_TAG, "Album art object not created yet, cannot set cover art");
    }

    LvglManager::lvgl_unlock();
}

void UiMusicPlayer::setPlayTime(uint32_t play_time_ms)
{
    if(LvglManager::lvgl_lock(_lock_timeout_ms) == false) {
        ESP_LOGE(_MP_TAG, "Failed to acquire LVGL lock to set cover art");
        return;
    }
    _song_play_time_ms = play_time_ms;

    lv_slider_set_range(_play_slider, 0, (int32_t)_song_play_time_ms);
    LvglManager::lvgl_unlock();
}

void UiMusicPlayer::setPlayPosition(uint32_t play_pos_ms)
{
    if(LvglManager::lvgl_lock(_lock_timeout_ms) == false) {
        ESP_LOGE(_MP_TAG, "Failed to acquire LVGL lock to set play position");
        return;
    }

    _cur_play_pos_ms = play_pos_ms;
    if(_play_slider) {
        lv_slider_set_value(_play_slider, (int32_t)_cur_play_pos_ms, LV_ANIM_OFF);
    }
    LvglManager::lvgl_unlock();
}

void UiMusicPlayer::regPlayCtrlCallback(PlayCtrlCallback cb)
{
    _play_ctrl_cb = cb;
    ESP_LOGI(_MP_TAG, "Registering Play Control Callback %x", (uintptr_t)cb.target<void(*)(uint16_t)>());

}

void UiMusicPlayer::play_ctrl_event_cb(lv_event_t * e)
{
    play_ctrl_param_t ctrl_param;

    int id = (int)lv_event_get_user_data(e);
    if (id == 1) {
            // ESP_LOGI(_MP_TAG, "play/pause Button Clicked!");
        lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
        lv_obj_t * label = lv_obj_get_child(btn, 0);

        // Toggle between Play and Pause icons
        const char * txt = lv_label_get_text(label);
        if(strcmp(txt, LV_SYMBOL_PAUSE) == 0) {
            lv_label_set_text(label, LV_SYMBOL_PLAY);
            ctrl_param.cmd = playControlCmd::PAUSE;
            UiMusicPlayer::instance()._play_ctrl_cb(ctrl_param);
        } else {
            lv_label_set_text(label, LV_SYMBOL_PAUSE);
            ctrl_param.cmd = playControlCmd::PLAY;
            UiMusicPlayer::instance()._play_ctrl_cb(ctrl_param);
        }

    } else if (id == 2) {
        // ESP_LOGI(_MP_TAG, "prev Button Clicked!");
        ctrl_param.cmd = playControlCmd::PREVIOUS;
        UiMusicPlayer::instance()._play_ctrl_cb(ctrl_param);
    } else if (id == 3) {
        // ESP_LOGI(_MP_TAG, "next Button Clicked!");
        ctrl_param.cmd = playControlCmd::NEXT;
        UiMusicPlayer::instance()._play_ctrl_cb(ctrl_param);
    } else {
        ESP_LOGW(_MP_TAG, "Unknown Button Clicked with id: %d", id);
    }
}

void UiMusicPlayer::handleVisualSwitchEvent(lv_event_t * e)
{
    ESP_LOGI(_MP_TAG, "Visual switch event triggered");

    if (_visual_type == visualType::ALBUM_ART) {
        lv_obj_add_flag(_album_art, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_sp_cont, LV_OBJ_FLAG_HIDDEN);
        _visual_type = visualType::SPECTRUM;
    } else if (_visual_type == visualType::SPECTRUM) {
        lv_obj_clear_flag(_album_art, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_sp_cont, LV_OBJ_FLAG_HIDDEN);
        _visual_type = visualType::ALBUM_ART;
    }
}

void UiMusicPlayer::visual_switch_event_cb(lv_event_t * e)
{
    ESP_LOGI(_MP_TAG, "Visual switch event triggered");

    UiMusicPlayer::instance().handleVisualSwitchEvent(e);
}

void UiMusicPlayer::audioVisual(const uint8_t *data, size_t size)
{
    if(_visual_type != visualType::SPECTRUM) {
        // ESP_LOGI(_MP_TAG, "Audio visual data received but current visual type is not SPECTRUM. Ignoring.");
        return;
    }

    if (_ringbuf_fft == nullptr) {
        ESP_LOGE(_MP_TAG, "Ring buffer for FFT is not initialized");
        return;
    }

    // A byte ring buffer can only hold an item as large as itself, and the file
    // decoder hands us chunks of several KB that it grows on demand. Queue the
    // audio in block-sized pieces: sending the caller's buffer whole would fail
    // outright and starve the analyzer with no visible symptom.
    static bool warned_full = false;
    size &= ~(size_t)3;   // whole stereo int16 samples only

    // Only the newest window can ever be displayed, so if the producer hands us
    // more than the ring buffer holds, keep its tail rather than its head.
    if (size > FFT_RINGBUF_SIZE) {
        data += size - FFT_RINGBUF_SIZE;
        size = FFT_RINGBUF_SIZE;
    }

    while (size > 0) {
        const size_t chunk = (size > FFT_BLOCK_BYTES) ? FFT_BLOCK_BYTES : size;

        if (xRingbufferSend(_ringbuf_fft, data, chunk, (TickType_t)0) != pdTRUE) {
            // The analyzer is behind, not audio playback. The visualisation only
            // ever shows the newest window, so drop the rest and keep going.
            if (!warned_full) {
                warned_full = true;
                ESP_LOGW(_MP_TAG, "FFT ring buffer full, dropping audio");
            }
            break;
        }

        data += chunk;
        size -= chunk;
    }
}

void UiMusicPlayer::fftProcessingTask() {

    // Size the tables for the transform we actually run. The tables scale with
    // the init size, so initialising at CONFIG_DSP_MAX_FFT_SIZE would spend
    // ~21 KB of RAM on tables a 256-point transform never touches.
    if (dsps_fft2r_init_fc32(nullptr, FFT_SIZE) != ESP_OK) {
        ESP_LOGE(_MP_TAG, "Not possible to initialize FFT for %d points", FFT_SIZE);
        return;
    }

    if (_fft_window == nullptr || _fft_io_buffer == nullptr || _fft_block == nullptr) {
        ESP_LOGE(_MP_TAG, "FFT working buffers unavailable, analyzer not started");
        dsps_fft2r_deinit_fc32();
        return;
    }

    // Generate hann window
    dsps_wind_hann_f32(_fft_window, FFT_SIZE);

    calculateBandEnds();

    int block_samples = 0;   // stereo samples currently held in _fft_block
    int64_t last_frame_us = 0;

    while (_fft_running.load()) {

        size_t item_size = 0;
        // The timeout is what lets this task notice both a stalled stream and
        // a shutdown request, so no separate semaphore is needed.
        uint8_t *item = static_cast<uint8_t *>(
            xRingbufferReceive(_ringbuf_fft, &item_size, pdMS_TO_TICKS(100)));
        if (item == nullptr || item_size == 0) {
            continue;
        }

        // Whole stereo int16 frames only; a trailing partial frame would
        // desync the accumulator.
        const uint8_t *chunk = item;
        const size_t chunk_size = item_size & ~(size_t)3;
        size_t offset = 0;

        while (offset < chunk_size) {
            const size_t room = (size_t)(FFT_SIZE - block_samples) * sizeof(int16_t) * 2;
            size_t take = chunk_size - offset;
            if (take > room) {
                take = room;
            }

            memcpy(&_fft_block[block_samples * 2], &chunk[offset], take);

            offset += take;
            block_samples += take / 4;

            if (block_samples < FFT_SIZE) {
                continue;
            }
            block_samples = 0;

            // Rate gate. Any block that arrives faster than the display can show
            // is dropped rather than analysed: the visualisation only ever
            // displays the newest window, so the older ones are wasted work.
            const int64_t now = esp_timer_get_time();
            if (_fft_running.load() && (now - last_frame_us) >= FFT_FRAME_INTERVAL_US) {
                last_frame_us = now;
                processFftBlock();
            }
        }

        vRingbufferReturnItem(_ringbuf_fft, item);
    }

    dsps_fft2r_deinit_fc32();
}

void UiMusicPlayer::processFftBlock() {

    // Downmix to mono, apply the window, and lay the result out as complex input.
    // The mix stays in integer: (l + r) >> 1 is exact for int16 and avoids a
    // float divide per sample.
    const int16_t *pcm16 = _fft_block;
    for (int i = 0; i < FFT_SIZE; i++) {
        const float mono = (float)((pcm16[i * 2] + pcm16[i * 2 + 1]) >> 1);
        _fft_io_buffer[i * 2]     = mono * _fft_window[i];
        _fft_io_buffer[i * 2 + 1] = 0.0f;
    }

    dsps_fft2r_fc32(_fft_io_buffer, FFT_SIZE);
    dsps_bit_rev_fc32(_fft_io_buffer, FFT_SIZE);

    // Walk the bins once, accumulating energy per band. Band ends tile the bins
    // exactly, so there is no magnitude array and no bounds test per bin.
    int band = 0;
    float energy_sum = 0.0f;
    int bins = 0;

    for (int bin = 1; bin < FFT_NUM_BINS; bin++) {
        const float real = _fft_io_buffer[bin * 2];
        const float imag = _fft_io_buffer[bin * 2 + 1];
        energy_sum += real * real + imag * imag;
        bins++;

        if (bin < _band_bin_end[band]) {
            continue;
        }

        applyBandEnergy(band, energy_sum, bins);
        energy_sum = 0.0f;
        bins = 0;
        band++;
    }

    // Trailing bins, in case the band table did not reach the end.
    if (bins > 0 && band < CONFIG_UI_SPECTRUM_BANDS_NUMS) {
        applyBandEnergy(band, energy_sum, bins);
    }
}

void UiMusicPlayer::applyBandEnergy(int band, float energy_sum, int bins) {
    // One sqrt per band rather than one per bin: sqrtf is a software routine on
    // the ESP32's Xtensa core, and this also lets bands react to transients
    // instead of averaging them away.
    const float rms = sqrtf(energy_sum / bins);
    const float dB = 20.0f * log10f(rms + 1e-4f);

    float normalized = dB / 120.0f;
    if (normalized > 1.0f) normalized = 1.0f;
    if (normalized < 0.0f) normalized = 0.0f;

    s_current_fft_bands[band] = (int16_t)(normalized * 120.0f);
}

void UiMusicPlayer::calculateBandEnds(void) {
    // Logarithmic curve over the bins that actually exist. The previous table
    // was scaled for 256 bins while only FFT_NUM_BINS of them are produced, so
    // the top of the spectrum was never summed and the high bands sat at zero.
    const float base = 1.2f;
    const int last_bin = FFT_NUM_BINS - 1;

    int last_end = 0;  // Skip bin 0 (DC offset)

    for (int b = 0; b < CONFIG_UI_SPECTRUM_BANDS_NUMS; b++) {
        // The last band absorbs everything the curve leaves over.
        if (b == CONFIG_UI_SPECTRUM_BANDS_NUMS - 1) {
            _band_bin_end[b] = (uint16_t)last_bin;
            break;
        }

        float progress = (float)(b + 1) / CONFIG_UI_SPECTRUM_BANDS_NUMS;
        int current_end = (int)roundf(base * powf((float)FFT_NUM_BINS / base, progress));

        // Boundaries safety check: every band owns at least one bin.
        if (current_end > last_bin) current_end = last_bin;
        if (current_end <= last_end) current_end = last_end + 1;

        _band_bin_end[b] = (uint16_t)current_end;

        last_end = current_end;
    }
}

void UiMusicPlayer::initBatteryAdc() {
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
    };
    adc_oneshot_new_unit(&init_config, &_adc1_handle);

    adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_oneshot_config_channel(_adc1_handle, ADC_CHANNEL_7, &chan_config);
}

float UiMusicPlayer::getRealBatteryVoltage() {
    int adc_raw = 0;
    adc_oneshot_read(_adc1_handle, ADC_CHANNEL_7, &adc_raw);

    float pin_v = (adc_raw / 4095.0f) * 3.3f;
    float bat_v = pin_v * 8.0f * 1.031f; 

    return bat_v;
}

void UiMusicPlayer::batteryVoltageReadTask() {
    while (true) {
        static float filtered_voltage = -1.0f;

        float current_raw_voltage = getRealBatteryVoltage();

        if (filtered_voltage < 0) {
            filtered_voltage = current_raw_voltage;
        } else {
            filtered_voltage = (filtered_voltage * 0.95f) + (current_raw_voltage * 0.05f);
        }

        int percentage = (int)getBatteryPercentage(filtered_voltage);
        char voltage_str[32];
        snprintf(voltage_str, sizeof(voltage_str), "%d%%", percentage);

        if(LvglManager::lvgl_lock(_lock_timeout_ms) == false) {
            ESP_LOGE(_MP_TAG, "Failed to acquire LVGL lock to set play position");
            continue;
        }
        int target_width = (percentage * 16) / 100;
    
        if (target_width <= 0 && percentage > 0) target_width = 1;

        lv_obj_set_width(_battery_bar, target_width);

        if (percentage <= 20) {
            lv_obj_set_style_bg_color(_battery_bar, lv_palette_main(LV_PALETTE_RED), 0);
        } else if (percentage <= 35) {
            lv_obj_set_style_bg_color(_battery_bar, lv_palette_main(LV_PALETTE_ORANGE), 0);
        } else {
            lv_obj_set_style_bg_color(_battery_bar, lv_palette_main(LV_PALETTE_GREEN), 0);
        }
        LvglManager::lvgl_unlock();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

float UiMusicPlayer::getBatteryPercentage(float voltage) {
    if (voltage >= 16.70f) return 100.0f;

    if (voltage <= 11.20f) return 0.0f;

    if (voltage > 16.0f) {
        return 90.0f + (voltage - 16.0f) * (10.0f / 0.7f);
    } 
    else if (voltage > 15.2f) {
        return 75.0f + (voltage - 15.2f) * (15.0f / 0.8f);
    } 
    else if (voltage > 14.8f) {
        return 50.0f + (voltage - 14.8f) * (25.0f / 0.4f);
    } 
    else if (voltage > 14.2f) {
        return 30.0f + (voltage - 14.2f) * (20.0f / 0.6f);
    } 
    else if (voltage > 13.2f) {
        return 15.0f + (voltage - 13.2f) * (15.0f / 1.0f);
    } 
    else {
        return 0.0f + (voltage - 11.2f) * (15.0f / 2.0f);
    }
}
