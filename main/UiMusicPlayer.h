#ifndef __UI_MUSIC_PLAYER_H__
#define __UI_MUSIC_PLAYER_H__

#include <functional>
#include "lvgl.h"

class UiMusicPlayer {
public:
    enum class playControlCmd : uint16_t {
        PLAY = 1,
        PAUSE = 2,
        PREVIOUS = 3,
        NEXT = 4,
    };
    typedef struct {
        playControlCmd cmd;
        uint8_t param[128];
    } play_ctrl_param_t;

    using PlayCtrlCallback = std::function<void(play_ctrl_param_t)>;

    UiMusicPlayer();
    void create_ui();
    void setTitle(const char *title);
    void setArtist(const char *artist);
    void setPlaying(bool playing);
    void setCoverArt(const uint8_t *data);
    void setPlayTime(uint32_t play_time_ms);
    void setPlayPosition(uint32_t play_pos_ms);

    void regPlayCtrlCallback(PlayCtrlCallback cb);

private:
    static UiMusicPlayer *s_instance;
    static constexpr const char *_MP_TAG = "UI_MPLAYER";

    static void play_ctrl_event_cb(lv_event_t * e);

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
    PlayCtrlCallback _play_ctrl_cb = nullptr;

    void handlePlayCtrlEvent(lv_event_t * e);
};

#endif // __UI_MUSIC_PLAYER_H__