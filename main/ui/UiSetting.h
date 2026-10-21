#ifndef __UI_SETTING_H__
#define __UI_SETTING_H__

#include "lvgl.h"
#include "Setting.h"

class UiSetting {
public:
    static UiSetting& instance() {
        static UiSetting instance;
        return instance;
    }

    // 打开设置列表页（含各配置项入口）
    void createSettingPage();

private:
    static constexpr const char *_SET_TAG = "UI_SETTING";

    // 设置列表页
    static void settingBackBtnEventCb(lv_event_t * e);
    static void settingGestureCb(lv_event_t * e);
    static void settingItemEventCb(lv_event_t * e);
    static void albumArtSwitchEventCb(lv_event_t * e);

    // 专辑封面显示开关：写入 NVS 并立即应用到播放页
    void toggleAlbumArt();
    void updateAlbumArtView();

    // Audio Source 协议选择子页
    static void audioSourceBackBtnEventCb(lv_event_t * e);
    static void audioSourceGestureCb(lv_event_t * e);
    static void protocolSelectEventCb(lv_event_t * e);
    // 切换协议后的全屏重启提示
    static void showRestartOverlay(AudioSource source);

    void openAudioSourcePage();
    void closeAudioSourcePage();
    void closeSettingPage();
    void updateProtocolView();

    // 当前选中的音频源
    AudioSource _current_audio_source = AudioSource::A2DP;

    lv_obj_t * _setting_page = nullptr;
    lv_obj_t * _audio_source_page = nullptr;
    // 设置列表页中的封面显示开关
    lv_obj_t * _album_art_switch = nullptr;
    // Audio Source 子页中的两个协议卡片
    lv_obj_t * _btn_bt = nullptr;
    lv_obj_t * _btn_airplay = nullptr;
    // 子页提示语，切换协议后显示 “Restarting...”
    lv_obj_t * _source_tip_label = nullptr;
    bool _restarting = false;
};

#endif // __UI_SETTING_H__