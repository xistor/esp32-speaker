#ifndef __UI_SETTING_H__
#define __UI_SETTING_H__

#include "lvgl.h"

class UiSetting {
public:
    static UiSetting& instance() {
        static UiSetting instance;
        return instance;
    }

    void createSettingPage();

private:
    static constexpr const char *_SET_TAG = "UI_SETTING";
    static void setting_event_cb(lv_event_t * e);
    static void settingBackBtnEventCb(lv_event_t * e);
    // 当前选中的协议：0-蓝牙, 1-AirPlay
    int _current_protocol = 0; 

    static void back_btn_event_cb(lv_event_t * e);
    static void protocolSelectEventCb(lv_event_t * e);

    lv_obj_t * _setting_page = nullptr;
    lv_obj_t *_setting_icon = nullptr;

    int _lock_timeout_ms = 500;
};

#endif // __UI_SETTING_H__