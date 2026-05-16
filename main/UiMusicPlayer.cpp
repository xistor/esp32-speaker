#include "esp_err.h"
#include "esp_log.h"
#include "UiMusicPlayer.h"
#include "LvglManager.h"

extern "C" {
    LV_FONT_DECLARE(Noto_14);
    LV_FONT_DECLARE(awsome_14);
}

UiMusicPlayer *UiMusicPlayer::s_instance = nullptr;

UiMusicPlayer::UiMusicPlayer()
{
    s_instance = this;

}

void UiMusicPlayer::create_ui()
{

    if(LvglManager::lvgl_lock(_lock_timeout_ms) == false) {
        ESP_LOGE(_MP_TAG, "Failed to acquire LVGL lock to create UI");
        return;
    }

    // 1. Background Container
    lv_obj_t * main_cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(main_cont, 240, 320);
    lv_obj_set_style_bg_color(main_cont, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(main_cont, 0, 0);
    lv_obj_set_style_radius(main_cont, 0, 0);
    lv_obj_remove_flag(main_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(main_cont);

    // 2. Album Art - Now at the TOP
     _album_art = lv_image_create(main_cont);
    lv_obj_set_size(_album_art, 130, 130);
    lv_obj_set_style_bg_color(_album_art, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_border_width(_album_art, 0, 0);
    lv_obj_set_style_pad_all(_album_art, 0, 0);
    lv_obj_align(_album_art, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_clear_flag(_album_art, LV_OBJ_FLAG_SCROLLABLE);
    lv_image_set_src(_album_art, LV_SYMBOL_AUDIO);
    uint32_t scale_factor = (130 * 256) / 200; // Result is 166 (approx 65%)

    lv_image_set_scale(_album_art, scale_factor);

    // 4. (Optional) Set the pivot point to the center so it scales inward perfectly
    lv_image_set_pivot(_album_art, 100, 100); // Center of the original 200x200 image

    // 3. Song Title - Positioned BELOW Album Art
    _title = lv_label_create(main_cont);
    lv_label_set_text(_title, "No Song Playing");
    lv_obj_set_width(_title, CONFIG_LCD_H_RES);
    lv_obj_set_style_text_color(_title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_long_mode(_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_title, &awsome_14, 0);
    lv_obj_align_to(_title, _album_art, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    // 4. Artist Name - Positioned BELOW Song Title
    _artist = lv_label_create(main_cont);
    lv_label_set_text(_artist, "No Artist");
    lv_obj_set_width(_artist, CONFIG_LCD_H_RES);
    lv_obj_set_height(_artist, 20);
    lv_obj_set_style_text_color(_artist, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_align(_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(_artist, &awsome_14, 0);
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
    lv_obj_add_event_cb(btn_play, play_pause_event_cb, LV_EVENT_RELEASED, NULL);

    // --- prev ---
    lv_obj_t * btn_prev = lv_btn_create(main_cont);
    lv_obj_set_size(btn_prev, 60, 60);
    lv_obj_align_to(btn_prev, btn_play, LV_ALIGN_OUT_LEFT_MID, -15, 0);

    lv_obj_set_style_bg_opa(btn_prev, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(btn_prev, 0, 0);
    lv_obj_set_style_border_width(btn_prev, 0, 0);

    lv_obj_t * label_prev = lv_label_create(btn_prev);
    lv_label_set_text(label_prev, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(label_prev, &awsome_14, 0);
    lv_obj_set_style_text_color(label_prev, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label_prev);

    // --- next ---
    lv_obj_t * btn_next = lv_btn_create(main_cont);
    lv_obj_set_size(btn_next, 60, 60);
    lv_obj_align_to(btn_next, btn_play, LV_ALIGN_OUT_RIGHT_MID, 15, 0);

    lv_obj_set_style_bg_opa(btn_next, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(btn_next, 0, 0);
    lv_obj_set_style_border_width(btn_next, 0, 0);

    lv_obj_t * label_next = lv_label_create(btn_next);
    lv_label_set_text(label_next, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(label_next, &awsome_14, 0);
    lv_obj_set_style_text_color(label_next, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label_next);

    LvglManager::lvgl_unlock();
}

void UiMusicPlayer::play_pause_event_cb(lv_event_t * e)
{
    if(s_instance) {
        s_instance->handlePlayPauseEvent(e);
    }
}

void UiMusicPlayer::handlePlayPauseEvent(lv_event_t * e)
{
    lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t * label = lv_obj_get_child(btn, 0);

    ESP_LOGI(_MP_TAG, "Play/Pause Button Clicked!");
    // Toggle between Play and Pause icons
    const char * txt = lv_label_get_text(label);
    if(strcmp(txt, LV_SYMBOL_PLAY) == 0) {
        lv_label_set_text(label, LV_SYMBOL_PAUSE);
    } else {
        lv_label_set_text(label, LV_SYMBOL_PLAY);
    }

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
        

    lv_image_set_src(_album_art, &_cover_img_dsc);
    lv_obj_invalidate(_album_art);
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