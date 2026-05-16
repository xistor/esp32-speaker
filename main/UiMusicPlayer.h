#ifndef __UI_MUSIC_PLAYER_H__
#define __UI_MUSIC_PLAYER_H__

#include "lvgl.h"

class UiMusicPlayer {
public:
    UiMusicPlayer();
    void create_ui();
    void setTitle(const char *title);
    void setArtist(const char *artist);
    void setPlaying(bool playing);
    void setCoverArt(const uint8_t *data);
    void setPlayTime(uint32_t play_time_ms);
    void setPlayPosition(uint32_t play_pos_ms);

private:
    static UiMusicPlayer *s_instance;
    static constexpr const char *_MP_TAG = "UI_MPLAYER";

    static void play_pause_event_cb(lv_event_t * e);

    lv_obj_t *_album_art = nullptr;
    lv_obj_t *_title = nullptr;
    lv_obj_t *_artist = nullptr;
    lv_obj_t *_album_img = nullptr;
    lv_obj_t * _play_label = nullptr;
    lv_obj_t * _play_slider = nullptr;

    lv_font_t * _font = nullptr;
    lv_anim_t _rotate_anim;
    lv_image_dsc_t _cover_img_dsc = {0};

    uint32_t _song_play_time_ms = 0;
    uint32_t _cur_play_pos_ms = 0;

    int _lock_timeout_ms = 500;
    void handlePlayPauseEvent(lv_event_t * e);

};

#endif // __UI_MUSIC_PLAYER_H__