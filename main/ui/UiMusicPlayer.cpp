#include "esp_err.h"
#include "esp_log.h"
#include "UiMusicPlayer.h"
#include "Setting.h"
#include "LvglManager.h"
#include "SpeakerApp.h"
#include "colorDef.h"
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

namespace {
/* Geometry of bar `index` for a given height, shared by the renderer and by
 * the dirty-area calculation so the two can never disagree. */
void spectrumBarRect(const lv_area_t *coords, int32_t index, int32_t bar_h,
                     lv_area_t *rect)
{
    const int32_t bands = CONFIG_UI_SPECTRUM_BANDS_NUMS;
    int32_t slot = lv_area_get_width(coords) / bands;
    if(slot < 1) {
        slot = 1;
    }
    const int32_t bar_width = (slot > 1) ? (slot - 1) : 1;
    // Centre the bar block when the width does not divide evenly, so the
    // leftover pixels are split instead of dumped on one side.
    const int32_t leftover = lv_area_get_width(coords) - slot * bands;

    rect->x1 = coords->x1 + leftover / 2 + index * slot;
    rect->x2 = rect->x1 + bar_width - 1;
    rect->y2 = coords->y2;
    rect->y1 = rect->y2 - bar_h + 1;
}
} // namespace

void UiMusicPlayer::sp_timer_cb(lv_timer_t * timer) {

    UiMusicPlayer &self = UiMusicPlayer::instance();
    if(self._sp_cont == nullptr) {
        return;
    }

    // The levels are published by the FFT task in bursts; easing towards them
    // here (rather than snapping the widget height to each new value) is what
    // makes the spectrum look continuous. dt is measured so the motion stays
    // identical when LVGL's timer handler is late.
    int64_t now = esp_timer_get_time();
    float dt = (self._sp_last_us == 0)
                   ? 0.033f
                   : (float)(now - self._sp_last_us) / 1000000.0f;
    self._sp_last_us = now;
    if(dt < 0.004f) dt = 0.004f;
    if(dt > 0.100f) dt = 0.100f;

    constexpr float kAttackTau = 0.030f;   // ~90 % of a jump in 70 ms
    constexpr float kReleaseTau = 0.180f;  // VU-meter-like fall

    lv_area_t coords;
    lv_obj_get_coords(self._sp_cont, &coords);
    const int32_t max_height = lv_area_get_height(&coords);

    lv_area_t dirty = {};
    bool has_dirty = false;

    for(int i = 0; i < CONFIG_UI_SPECTRUM_BANDS_NUMS; i++) {
        float target = (float)self.s_current_fft_bands[i];
        if(target > (float)max_height) target = (float)max_height;
        if(target < 2.0f) target = 0.0f;   // gate the noise floor

        float current = self._sp_bar_h[i];
        const float tau = (target > current) ? kAttackTau : kReleaseTau;
        const float alpha = 1.0f - expf(-dt / tau);

        float next = current + (target - current) * alpha;
        if(next < 0.5f) next = 0.0f;
        self._sp_bar_h[i] = next;

        const int32_t px = (int32_t)(next + 0.5f);
        const int32_t drawn = self._sp_drawn_h[i];
        if(px == drawn) {
            continue;
        }
        self._sp_drawn_h[i] = (int16_t)px;

        // Only the strip between the old and the new top changed: everything
        // below min(old, new) is still covered by the bar.
        const int32_t lo = (px < drawn) ? px : drawn;
        const int32_t hi = (px > drawn) ? px : drawn;
        lv_area_t hi_rect, lo_rect;
        spectrumBarRect(&coords, i, hi, &hi_rect);
        spectrumBarRect(&coords, i, lo, &lo_rect);

        lv_area_t band;
        band.x1 = hi_rect.x1;
        band.x2 = hi_rect.x2;
        band.y1 = hi_rect.y1;
        band.y2 = (lo > 0) ? lo_rect.y1 - 1 : coords.y2;

        if(!has_dirty) {
            dirty = band;
            has_dirty = true;
        } else {
            dirty.x1 = LV_MIN(dirty.x1, band.x1);
            dirty.y1 = LV_MIN(dirty.y1, band.y1);
            dirty.x2 = LV_MAX(dirty.x2, band.x2);
            dirty.y2 = LV_MAX(dirty.y2, band.y2);
        }
    }

    if(has_dirty) {
        // Invalidating only the changed strip keeps the number of clip
        // regions (and therefore draw calls) small.
        lv_obj_invalidate_area(self._sp_cont, &dirty);
    }
}

void UiMusicPlayer::spectrumDrawEventCb(lv_event_t * e)
{
    UiMusicPlayer &self = UiMusicPlayer::instance();

    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if(obj == nullptr || layer == nullptr) {
        return;
    }

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    const int32_t height = lv_area_get_height(&coords);
    if(height <= 0) {
        return;
    }

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(0x00A2FF);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = 0;

    for(int32_t i = 0; i < CONFIG_UI_SPECTRUM_BANDS_NUMS; i++) {
        int32_t bar = self._sp_drawn_h[i];
        if(bar <= 0) {
            continue;
        }
        if(bar > height) {
            bar = height;
        }

        lv_area_t rect;
        spectrumBarRect(&coords, i, bar, &rect);
        lv_draw_rect(layer, &dsc, &rect);
    }
}

void UiMusicPlayer::statusBarGestureCb(lv_event_t * e){
    ESP_LOGI(_MP_TAG, "Status bar gesture detected");

    lv_indev_t * indev = lv_indev_active();
    if(indev == nullptr) return;

    // Only react to a swipe (drag) downwards started on the status bar
    if(lv_indev_get_gesture_dir(indev) == LV_DIR_BOTTOM) {
        ESP_LOGI(_MP_TAG, "Swipe down on status bar, opening settings page");
        UiMusicPlayer::instance()._ui_setting.createSettingPage();
    }
}

void UiMusicPlayer::statusTimerCb(lv_timer_t * timer)
{
    UiMusicPlayer &self = UiMusicPlayer::instance();
    if(self._bt_icon == nullptr || self._airplay_icon == nullptr) {
        return;
    }

    SpeakerApp &app = SpeakerApp::instance();
    const bool bt = app.isBtConnected();
    const bool airplay = app.isAirPlayConnected();

    // Writing a local style on a label wakes the LVGL renderer, so only touch
    // the widgets when the state actually flips.
    if(bt != self._bt_connected_shown) {
        self._bt_connected_shown = bt;
        lv_obj_set_style_text_color(self._bt_icon,
                                    bt ? COLOR_SOURCE_ON : COLOR_SOURCE_OFF, 0);
    }
    if(airplay != self._airplay_connected_shown) {
        self._airplay_connected_shown = airplay;
        lv_obj_set_style_text_color(self._airplay_icon,
                                    airplay ? COLOR_SOURCE_ON : COLOR_SOURCE_OFF, 0);
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

    // 1.0.1 连接状态：蓝牙 / AirPlay。图标随状态由 statusTimerCb 刷新，
    //       作为 status_bar 的子对象，滑动仍然能触发下拉设置。
    //       注意不要用 awesome_14：它只包含 0xF000-0xF0FF，而这两个符号
    //       在 0xF1EB / 0xF293，只有内置的 Montserrat 14 才有对应字形。
    _bt_icon = lv_label_create(status_bar);
    lv_label_set_text(_bt_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_color(_bt_icon, COLOR_SOURCE_OFF, 0);
    lv_obj_align(_bt_icon, LV_ALIGN_RIGHT_MID, -64, 0);

    _airplay_icon = lv_label_create(status_bar);
    lv_label_set_text(_airplay_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(_airplay_icon, COLOR_SOURCE_OFF, 0);
    lv_obj_align(_airplay_icon, LV_ALIGN_RIGHT_MID, -34, 0);

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

    lv_obj_remove_flag(_sp_cont, LV_OBJ_FLAG_SCROLLABLE);

    // All bars are painted by one draw callback. Twenty-four child objects
    // meant a flex re-layout plus 24 style writes on every animation frame,
    // which showed up as visible stepping on this CPU.
    lv_obj_add_event_cb(_sp_cont, spectrumDrawEventCb, LV_EVENT_DRAW_MAIN, NULL);

    lv_obj_add_flag(_sp_cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_sp_cont, visual_switch_event_cb, LV_EVENT_CLICKED, NULL);

    // 20 fps is smooth enough for the bars and leaves the software renderer
    // time to finish text glyphs; the earlier 30 ms period kept swdraw busy
    // almost continuously and starved IDLE0 (task watchdog).
    lv_timer_create(sp_timer_cb, 50, NULL);
    // 连接状态一秒轮询一次即可，避免为每次 A2DP/RTSP 事件添加回调链
    lv_timer_create(statusTimerCb, 1000, NULL);
    lv_obj_add_flag(_sp_cont, LV_OBJ_FLAG_HIDDEN); 

    // 3. Song Title - Positioned BELOW Album Art
    _title = lv_label_create(_main_page);
    lv_label_set_text(_title, "No Song Playing");
    lv_obj_set_width(_title, CONFIG_LCD_H_RES);
    lv_obj_set_style_text_color(_title, lv_color_hex(0xFFFFFF), 0);
    // Plain truncation instead of a circular scroll: the scrolling label is
    // an animation that redraws itself every refresh, and with QQ Music
    // pushing a new lyric line into the title every couple of seconds that
    // kept the software renderer busy non-stop on this CPU.
    lv_label_set_long_mode(_title, LV_LABEL_LONG_DOT);
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

    // Apply the persisted preference now that the visual widgets exist.
    setAlbumArtEnabled(Setting::instance().getShowAlbumArt());
}

void UiMusicPlayer::setTitle(const char *title)
{
    if(LvglManager::lvgl_lock(_lock_timeout_ms) == false) {
        ESP_LOGE(_MP_TAG, "Failed to acquire LVGL lock to set title");
        return;
    }

    if(_title && strcmp(lv_label_get_text(_title), title) != 0) {
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

    if(_artist && strcmp(lv_label_get_text(_artist), artist) != 0) {
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

    if(_play_label != nullptr) {
        lv_label_set_text(_play_label, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }

    LvglManager::lvgl_unlock();
}

void UiMusicPlayer::setAlbumArtEnabled(bool enabled)
{
    _album_art_enabled.store(enabled);

    if(enabled) {
        // The widget keeps whatever it shows now; the next cover art frame
        // replaces it when the sender delivers one.
        return;
    }

    // Turning cover art off must not change the player layout: the album art
    // area stays on screen and simply falls back to the bundled placeholder.
    if(LvglManager::lvgl_lock(100) == false) {
        ESP_LOGW(_MP_TAG, "Failed to acquire LVGL lock to reset cover art");
        return;
    }

    if(_album_art != nullptr) {
        const uint32_t scale = (130 * LV_SCALE_NONE) / 200;
        lv_image_set_scale(_album_art, scale);
        lv_image_set_pivot(_album_art, 100, 100);
        lv_image_set_src(_album_art, &default_albumart);
        lv_obj_invalidate(_album_art);
    }

    LvglManager::lvgl_unlock();
}

void UiMusicPlayer::setCoverArt(const uint8_t *p_data)
{
    setCoverArt(p_data, 200, 200);
}

void UiMusicPlayer::setCoverArt(const uint8_t *p_data, uint16_t width,
                                uint16_t height)
{
    if (p_data == nullptr || width == 0 || height == 0) {
        return;
    }
    // Cover art is turned off: the widget is hidden, so drop the frame and
    // spare the renderer the work.
    if (!_album_art_enabled.load()) {
        return;
    }
    // Cosmetic: skip the frame rather than block the decode worker behind a
    // busy renderer.
    if(LvglManager::lvgl_lock(100) == false) {
        ESP_LOGW(_MP_TAG, "Failed to acquire LVGL lock to set cover art");
        return;
    }
    _cover_img_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    _cover_img_dsc.header.cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
    _cover_img_dsc.header.w = width;
    _cover_img_dsc.header.h = height;
    _cover_img_dsc.data_size = static_cast<uint32_t>(width) * height * sizeof(uint16_t);
    _cover_img_dsc.data = p_data;

    if(_album_art) {
        // The widget is 130x130. Cover art decoded at 130x130 needs no
        // scaling; the 200x200 Bluetooth path keeps the old 0.65x factor.
        uint32_t scale = (130 * LV_SCALE_NONE) / width;
        if(scale > LV_SCALE_NONE) {
            scale = LV_SCALE_NONE;
        }
        lv_image_set_scale(_album_art, scale);
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

    if(_play_slider != nullptr) {
        lv_slider_set_range(_play_slider, 0, (int32_t)_song_play_time_ms);
    }
    LvglManager::lvgl_unlock();
}

void UiMusicPlayer::applyMetadata(const char *title, const char *artist, int durationMs)
{
    // Metadata is disposable and frequently updated by lyric-capable senders.
    // Do not block the artwork/UI worker for the full lock timeout.
    if(LvglManager::lvgl_lock(100) == false) {
        ESP_LOGD(_MP_TAG, "Skipping metadata update, renderer busy");
        return;
    }

    // Senders such as QQ Music repeat the same title/artist around lyric
    // lines. Re-setting identical text invalidates the label and makes the
    // software renderer redo expensive CJK glyph lookups.
    if(title && _title && strcmp(lv_label_get_text(_title), title) != 0) {
        lv_label_set_text(_title, title);
    }
    if(artist && _artist && strcmp(lv_label_get_text(_artist), artist) != 0) {
        lv_label_set_text(_artist, artist);
    }
    if(durationMs >= 0) {
        _song_play_time_ms = static_cast<uint32_t>(durationMs);
        if(_play_slider != nullptr) {
            lv_slider_set_range(_play_slider, 0, (int32_t)_song_play_time_ms);
        }
    }

    LvglManager::lvgl_unlock();
}

void UiMusicPlayer::setPlayPosition(uint32_t play_pos_ms)
{
    // Progress arrives several times per second and is disposable: never let
    // it block the worker for the full 500 ms lock timeout, otherwise a busy
    // renderer starves the same task that decodes cover art.
    if(LvglManager::lvgl_lock(50) == false) {
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
        // Start from a flat baseline instead of the levels frozen when the
        // spectrum was last visible.
        memset(_sp_bar_h, 0, sizeof(_sp_bar_h));
        memset(_sp_drawn_h, 0, sizeof(_sp_drawn_h));
        _sp_last_us = 0;
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
    int64_t last_data_us = esp_timer_get_time();

    while (_fft_running.load()) {

        size_t item_size = 0;
        // The timeout is what lets this task notice both a stalled stream and
        // a shutdown request, so no separate semaphore is needed.
        uint8_t *item = static_cast<uint8_t *>(
            xRingbufferReceive(_ringbuf_fft, &item_size, pdMS_TO_TICKS(100)));
        if (item == nullptr || item_size == 0) {
            // Paused, disconnected, or between tracks: let the bars fall
            // instead of freezing at the last analysed frame.
            if ((esp_timer_get_time() - last_data_us) > 300000) {
                memset(s_current_fft_bands, 0, sizeof(s_current_fft_bands));
            }
            continue;
        }
        last_data_us = esp_timer_get_time();

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

        applyBandEnergy(band, energy_sum);
        energy_sum = 0.0f;
        bins = 0;
        band++;
    }

    // Trailing bins, in case the band table did not reach the end.
    if (bins > 0 && band < CONFIG_UI_SPECTRUM_BANDS_NUMS) {
        applyBandEnergy(band, energy_sum);
    }

    publishBandLevels();
}

void UiMusicPlayer::applyBandEnergy(int band, float energy_sum) {
    // Total energy in the band, not the per-bin average. The log-spaced bands
    // hold a growing number of bins towards the top (1 bin at the bottom, ~28
    // at the top), so averaging divided the high bands by their own width and
    // made them read far lower than the low ones; summed band power matches
    // what a hardware spectrum analyser shows. A single-bin tone is unaffected.
    //
    // Undo the FFT and Hann-window gain here so the result is comparable with
    // the PCM full scale: a bin-centred sine of amplitude A yields |X| = A*N/4.
    const float band_amplitude = sqrtf(energy_sum);
    _band_amp[band] = band_amplitude * (4.0f / (float)FFT_SIZE);
}

void UiMusicPlayer::publishBandLevels() {
    constexpr float kFullScale = 32768.0f;
    // Peak band amplitude of loud, full-scale material. Anything at or above
    // this is displayed as-is; quieter streams are lifted towards it.
    constexpr float kRefAmp = 5200.0f;
    // Both sources are analysed before their output volume stage (AirPlay
    // scales in its pipeline, A2DP in the I2S task), so the input level is
    // independent of the listening volume. Allow enough make-up gain for
    // quiet material; the noise gate below stops it amplifying hiss.
    constexpr float kMaxGain = 24.0f;     // up to ~+27 dB
    constexpr float kSilenceAmp = 60.0f;  // ~-55 dBFS
    constexpr float kDisplayGainDb = 6.0f;
    constexpr float kFloorDb = -60.0f;
    constexpr int32_t kBarMax = 120;

    float peak = 0.0f;
    for (int i = 0; i < CONFIG_UI_SPECTRUM_BANDS_NUMS; i++) {
        if (_band_amp[i] > peak) {
            peak = _band_amp[i];
        }
    }

    // Fade the display out below the gate instead of switching it off: a hard
    // threshold combined with the make-up gain below would make fading tracks
    // pop from 0 px straight to mid-height.
    float silence_scale = 1.0f;
    if (peak < kSilenceAmp) {
        silence_scale = peak / kSilenceAmp;
    } else {
        // Slow level tracking. The bars must not follow the volume, and the
        // input level differs wildly between tracks. Normalising against a
        // slow peak makes quiet and loud material look the same; the release
        // is short enough that a level change settles in under a second, and
        // long enough not to squash the dynamics inside a track.
        constexpr float kAttackAlpha = 0.25f;   // ~65 ms to catch a louder stream
        constexpr float kReleaseAlpha = 0.02f;  // ~0.8 s to settle back down
        const float alpha = (peak > _agc_peak) ? kAttackAlpha : kReleaseAlpha;
        _agc_peak += (peak - _agc_peak) * alpha;
    }

    float gain = kRefAmp / _agc_peak;
    if (gain < 1.0f) gain = 1.0f;
    if (gain > kMaxGain) gain = kMaxGain;
    gain *= silence_scale;

    for (int i = 0; i < CONFIG_UI_SPECTRUM_BANDS_NUMS; i++) {
        const float amplitude = _band_amp[i] * gain;
        const float dbfs =
            20.0f * log10f(amplitude / kFullScale + 1e-9f) + kDisplayGainDb;

        // Map the musically useful -60..0 dBFS range onto the whole bar, so
        // the level has an absolute reference instead of hugging the top.
        float normalized = (dbfs - kFloorDb) * (-1.0f / kFloorDb);
        if (normalized > 1.0f) normalized = 1.0f;
        if (normalized < 0.0f) normalized = 0.0f;

        s_current_fft_bands[i] = (int16_t)(normalized * kBarMax);
    }
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

        // Battery level is cosmetic: never block the renderer for the full
        // lock timeout, and back off instead of retrying in a tight loop.
        if(LvglManager::lvgl_lock(50) == false) {
            vTaskDelay(pdMS_TO_TICKS(1000));
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
