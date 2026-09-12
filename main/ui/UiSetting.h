#ifndef __UI_SETTING_H__
#define __UI_SETTING_H__

#include "lvgl.h"

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

    // Audio Source 协议选择子页
    static void audioSourceBackBtnEventCb(lv_event_t * e);
    static void audioSourceGestureCb(lv_event_t * e);
    static void protocolSelectEventCb(lv_event_t * e);

    void openAudioSourcePage();
    void closeAudioSourcePage();
    void closeSettingPage();
    void updateProtocolView();

    // 当前选中的协议：0-蓝牙(A2DP), 1-AirPlay
    int _current_protocol = 0;

    lv_obj_t * _setting_page = nullptr;
    lv_obj_t * _audio_source_page = nullptr;
    // Audio Source 子页中的两个协议卡片
    lv_obj_t * _btn_bt = nullptr;
    lv_obj_t * _btn_airplay = nullptr;
};

#endif // __UI_SETTING_H__