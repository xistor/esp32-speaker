#include "esp_err.h"
#include "esp_log.h"
#include "UiMusicPlayer.h"
#include "LvglManager.h"
#include <cmath>
#include "esp_dsp.h"
#include "esp_pthread.h"

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

UiMusicPlayer *UiMusicPlayer::s_instance = nullptr;
int16_t UiMusicPlayer::s_current_fft_bands[CONFIG_UI_SPECTRUM_BANDS_NUMS];


UiMusicPlayer::UiMusicPlayer()
{
    s_instance = this;

    _ringbuf_fft = xRingbufferCreate(_fft_buf_size, RINGBUF_TYPE_BYTEBUF);
    if (_ringbuf_fft == nullptr) {
        ESP_LOGE(_MP_TAG, "Failed to create ring buffer for FFT data");
        return;
    }

    _fft_write_semaphore = xSemaphoreCreateBinary();

     if (_fft_write_semaphore == nullptr) {
        ESP_LOGE(_MP_TAG, "%s, semaphore create failed", __func__);
        return;
    }
}

UiMusicPlayer::~UiMusicPlayer()
{

    _fft_running.store(false);
    xSemaphoreGive(_fft_write_semaphore);

    if (_fft_process_thread.joinable()) {
        _fft_process_thread.join();
    }

    if(_ringbuf_fft) {
        vRingbufferDelete(_ringbuf_fft);
        _ringbuf_fft = nullptr;
    }
}

void UiMusicPlayer::sp_timer_cb(lv_timer_t * timer) {

    for(int i = 0; i < CONFIG_UI_SPECTRUM_BANDS_NUMS; i++) {
            lv_obj_set_height(s_instance->_band_objs[i], (int32_t)s_current_fft_bands[i]);
    }

}

void UiMusicPlayer::create_ui()
{

    if(LvglManager::lvgl_lock(_lock_timeout_ms) == false) {
        ESP_LOGE(_MP_TAG, "Failed to acquire LVGL lock to create UI");
        return;
    }

    // 1. Background Container
    lv_obj_t * main_cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(main_cont, CONFIG_LCD_H_RES, CONFIG_LCD_V_RES);
    lv_obj_set_style_bg_color(main_cont, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(main_cont, 0, 0);
    lv_obj_set_style_radius(main_cont, 0, 0);
    lv_obj_remove_flag(main_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(main_cont);

    // 1.1 battery
    _battery_area = lv_obj_create(main_cont);
    lv_obj_remove_style_all(_battery_area); 
    lv_obj_set_size(_battery_area, 55, 20);
    lv_obj_align(_battery_area, LV_ALIGN_TOP_RIGHT, -10, 5); 

    _battery_text = lv_label_create(_battery_area);
    lv_label_set_text(_battery_text, "--%");
    lv_obj_set_style_text_font(_battery_text, &awesome_14, 0); 
    lv_obj_align(_battery_text, LV_ALIGN_LEFT_MID, 0, 0); 

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
     _album_art = lv_image_create(main_cont);
    lv_obj_set_size(_album_art, 130, 130);
    lv_obj_set_style_bg_color(_album_art, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_border_width(_album_art, 0, 0);
    lv_obj_set_style_pad_all(_album_art, 0, 0);
    lv_obj_align(_album_art, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_clear_flag(_album_art, LV_OBJ_FLAG_SCROLLABLE);
    lv_image_set_src(_album_art, &default_albumart);
    uint32_t scale_factor = (130 * LV_SCALE_NONE) / 200; // Result is 166 (approx 65%)

    lv_image_set_scale(_album_art, scale_factor);
    lv_image_set_pivot(_album_art, 100, 100); // Center of the original 200x200 image

    lv_obj_add_flag(_album_art, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_album_art, visual_switch_event_cb, LV_EVENT_CLICKED, NULL);

    // 2.5 Spectrum Visualizer
    _sp_cont = lv_obj_create(main_cont);
    int _sp_cont_width = CONFIG_LCD_H_RES - 40;
    lv_obj_set_size(_sp_cont, _sp_cont_width, 120); 
    lv_obj_align(_sp_cont, LV_ALIGN_TOP_MID, 0, 24);

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
    _title = lv_label_create(main_cont);
    lv_label_set_text(_title, "No Song Playing");
    lv_obj_set_width(_title, CONFIG_LCD_H_RES);
    lv_obj_set_style_text_color(_title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_long_mode(_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_title, &awesome_14, 0);
    lv_obj_align_to(_title, _sp_cont, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    // 4. Artist Name - Positioned BELOW Song Title
    _artist = lv_label_create(main_cont);
    lv_label_set_text(_artist, "No Artist");
    lv_obj_set_width(_artist, CONFIG_LCD_H_RES);
    lv_obj_set_height(_artist, 20);
    lv_obj_set_style_text_color(_artist, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_align(_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(_artist, &awesome_14, 0);
    lv_obj_align_to(_artist, _title, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

    // 5. Progress Slider
    _play_slider = lv_slider_create(main_cont);
    lv_obj_set_width(_play_slider, 200);
    lv_obj_set_height(_play_slider, 6);
    lv_obj_align(_play_slider, LV_ALIGN_BOTTOM_MID, 0, -80);
    lv_obj_set_style_bg_color(_play_slider, lv_color_hex(0x1DB954), LV_PART_INDICATOR);

    // 6. contrl button
    lv_obj_t * btn_play = lv_btn_create(main_cont);
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
    lv_obj_t * btn_prev = lv_btn_create(main_cont);
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
    lv_obj_t * btn_next = lv_btn_create(main_cont);
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


    calculateBandWidths();
    _fft_running.store(true);

    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 1024 * 4;
    cfg.thread_name = "fftProcessingTask";
    cfg.pin_to_core = 1;
    esp_pthread_set_cfg(&cfg);

    _fft_process_thread = std::thread(&UiMusicPlayer::fftProcessingTask, this);
    _visual_type = visualType::ALBUM_ART;


    initBatteryAdc();
    _bat_volt_read_thread = std::thread(&UiMusicPlayer::batteryVoltageReadTask, this);
}

void UiMusicPlayer::setTitle(const char *title)
{
    if(LvglManager::lvgl_lock(_lock_timeout_ms) == false) {
        ESP_LOGE(_MP_TAG, "Failed to acquire LVGL lock to set title");
        return;
    }

    lv_label_set_text(_title, title);
    LvglManager::lvgl_unlock();
}

void UiMusicPlayer::setArtist(const char *artist)
{
    if(LvglManager::lvgl_lock(_lock_timeout_ms) == false) {
        ESP_LOGE(_MP_TAG, "Failed to acquire LVGL lock to set artist");
        return;
    }

    lv_label_set_text(_artist, artist);
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
    lv_slider_set_value(_play_slider, (int32_t)_cur_play_pos_ms, LV_ANIM_OFF);
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
            if(s_instance) {
                ctrl_param.cmd = playControlCmd::PAUSE;
                s_instance->_play_ctrl_cb(ctrl_param);
            }

        } else {
            lv_label_set_text(label, LV_SYMBOL_PAUSE);
             if(s_instance) {
                ctrl_param.cmd = playControlCmd::PLAY;
                s_instance->_play_ctrl_cb(ctrl_param);
            }
        }

    } else if (id == 2) {
        // ESP_LOGI(_MP_TAG, "prev Button Clicked!");
        if(s_instance) {
            ctrl_param.cmd = playControlCmd::PREVIOUS;
            s_instance->_play_ctrl_cb(ctrl_param);
        }
    } else if (id == 3) {
        // ESP_LOGI(_MP_TAG, "next Button Clicked!");
        if(s_instance) {
            ctrl_param.cmd = playControlCmd::NEXT;
            s_instance->_play_ctrl_cb(ctrl_param);
        }
    } else {
        ESP_LOGW(_MP_TAG, "Unknown Button Clicked with id: %d", id);
    }
}

void UiMusicPlayer::visual_switch_event_cb(lv_event_t * e)
{
    ESP_LOGI(_MP_TAG, "Visual switch event triggered");
    if(s_instance) {

        if (s_instance->_visual_type == visualType::ALBUM_ART) {
            lv_obj_add_flag(s_instance->_album_art, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_instance->_sp_cont, LV_OBJ_FLAG_HIDDEN);
            s_instance->_visual_type = visualType::SPECTRUM;
        } else if (s_instance->_visual_type == visualType::SPECTRUM) {
            lv_obj_clear_flag(s_instance->_album_art, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_instance->_sp_cont, LV_OBJ_FLAG_HIDDEN);
            s_instance->_visual_type = visualType::ALBUM_ART;
        }
    }
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

    if (xRingbufferSend(_ringbuf_fft, data, size, (TickType_t)0) != pdTRUE) {
        ESP_LOGW(_MP_TAG, "Failed to send audio data to FFT ring buffer");
    } else {
        xSemaphoreGive(_fft_write_semaphore);
    }
}

void UiMusicPlayer::fftProcessingTask() {

    esp_err_t ret;
    const int FFT_SIZE = CONFIG_UI_FFT_SAMPLE_SIZE;

    ret = dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE);
    if (ret  != ESP_OK) {
        ESP_LOGE(_MP_TAG, "Not possible to initialize FFT. Error = %i", ret);
        return;
    }

    // Generate hann window
    dsps_wind_hann_f32(_fft_window, FFT_SIZE);

    float magnitudes[FFT_SIZE / 2] = {0};

    while (_fft_running.load()) {

        if (pdTRUE == xSemaphoreTake(_fft_write_semaphore, portMAX_DELAY)) {

            while (_fft_running.load()) {
                size_t item_size = 0;
                uint8_t *audio_data = static_cast<uint8_t *>(xRingbufferReceiveUpTo
                    (_ringbuf_fft, &item_size, pdMS_TO_TICKS(3), FFT_SIZE * 4));
                
                if (audio_data == nullptr || item_size == 0) {
                    // ESP_LOGI(_MP_TAG, "No audio data received, waiting...");
                    break;
                }

                int16_t *pcm16 = (int16_t *)audio_data;
                for(int i = 0; i < FFT_SIZE; i++) {
                    int16_t left = pcm16[i * 2];
                    int16_t right = pcm16[i * 2 + 1];

                    float mono = (left + right) / 2.0f;

                    mono *= _fft_window[i];
                    _fft_io_buffer[i * 2]     = mono;
                    _fft_io_buffer[i * 2 + 1] = 0.0f;

                }
                vRingbufferReturnItem(_ringbuf_fft, audio_data);

                dsps_fft2r_fc32(_fft_io_buffer, FFT_SIZE);
                dsps_bit_rev_fc32(_fft_io_buffer, FFT_SIZE);

                for (int i = 0; i < FFT_SIZE / 2; i++) {
                    float real = _fft_io_buffer[i * 2];
                    float imag = _fft_io_buffer[i * 2 + 1];
                    magnitudes[i] = sqrtf(real * real + imag * imag);
                }


                int fft_index = 1;

                for (int b = 0; b < CONFIG_UI_SPECTRUM_BANDS_NUMS; b++) {
                    float sum = 0;
                    int width = _band_widths[b];
                    
                    for (int w = 0; w < width; w++) {
                        if (fft_index < FFT_SIZE / 2) {
                            sum += magnitudes[fft_index++];
                        }
                    }
                    float avg_magnitude = sum / width;

                    float dB = 20.0f * log10f(avg_magnitude + 1e-4f);

                    float normalized = dB / 120.0f; 

                    if (normalized > 1.0f) normalized = 1.0f;
                    if (normalized < 0.0f) normalized = 0.0f;

                    s_current_fft_bands[b] = normalized * 120;
                }
            }
        }
    }

}

void UiMusicPlayer::calculateBandWidths(void) {
    int last_end_index = 1;  // Skip bin 0 (DC offset)
    float base = 1.2f;       // Reduced base offset optimized for 64 splits

    for (int b = 0; b < CONFIG_UI_SPECTRUM_BANDS_NUMS; b++) {
        // Logarithmic progress curve to partition the 256 bins into 64 slices
        float progress = (float)(b + 1) / CONFIG_UI_SPECTRUM_BANDS_NUMS;
        int current_end_index = (int)roundf(base * powf(256.0f / base, progress));

        // Boundaries safety check
        if (current_end_index > 256) current_end_index = 256;
        if (current_end_index <= last_end_index) current_end_index = last_end_index + 1;

        // Calculate the width for the current bar
        _band_widths[b] = current_end_index - last_end_index;
        
        // Update marker
        last_end_index = current_end_index;
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
        lv_label_set_text(_battery_text, voltage_str);
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
        vTaskDelay(pdMS_TO_TICKS(5000));
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
