#include "UiSetting.h"
#include "LvglManager.h"
#include "esp_log.h"
#include "colorDef.h"
#include "NvsHelper.h"
#include "SpeakerApp.h"

/* 配置全屏页面：禁用滚动并取消手势上抛，使上滑手势能由页面自身捕获 */
static void configFullScreenPage(lv_obj_t *page, lv_event_cb_t gesture_cb)
{
    lv_obj_set_size(page, CONFIG_LCD_H_RES, CONFIG_LCD_V_RES);
    lv_obj_set_style_bg_color(page, COLOR_BACKGROUND, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(page, gesture_cb, LV_EVENT_GESTURE, nullptr);
    lv_obj_center(page);
}

/* 页面顶部标题栏：返回按钮 + 标题 */
static void buildPageHeader(lv_obj_t *parent, const char *title,
                            lv_event_cb_t back_cb)
{
    lv_obj_t *header = lv_obj_create(parent);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, CONFIG_LCD_H_RES, 40);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *btn_back = lv_btn_create(header);
    lv_obj_set_size(btn_back, 35, 35);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(btn_back, 0, 0);
    lv_obj_t *label_back = lv_label_create(btn_back);
    lv_label_set_text(label_back, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(label_back, lv_color_white(), 0);
    lv_obj_center(label_back);
    lv_obj_add_event_cb(btn_back, back_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *title_label = lv_label_create(header);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, COLOR_SETTING_TITLE_COLOR, 0);
    lv_obj_align(title_label, LV_ALIGN_CENTER, 0, 0);
}

/* 协议选择卡片 */
static lv_obj_t * createProtocolCard(lv_obj_t *parent, const char *text,
                                     lv_event_cb_t click_cb)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_height(card, 50);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, click_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 15, 0);

    return card;
}

void UiSetting::createSettingPage()
{
    if(_setting_page != nullptr) {
        ESP_LOGI(_SET_TAG, "Setting page already opened");
        return;
    }

    _setting_page = lv_obj_create(lv_scr_act());
    configFullScreenPage(_setting_page, settingGestureCb);
    buildPageHeader(_setting_page, "Settings", settingBackBtnEventCb);

    // 设置项列表容器（纵向排列，便于后续扩展更多配置项）
    lv_obj_t *list = lv_obj_create(_setting_page);
    lv_obj_set_size(list, CONFIG_LCD_H_RES - 40, CONFIG_LCD_V_RES - 40);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 55);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    // ---- 设置项：Audio Source（音频源），点击进入协议选择子页 ----
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 52);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_bg_color(row, COLOR_SETTING_BTN_UNSELECTED, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, settingItemEventCb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 15, 0);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, "Audio Source");
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 45, 0);

    // 右侧：进入箭头（当前协议在子页中查看与选择）
    lv_obj_t *arrow = lv_label_create(row);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, COLOR_SETTING_TEXT_COLOR, 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -12, 0);
}

void UiSetting::openAudioSourcePage()
{
    if(_audio_source_page != nullptr) {
        ESP_LOGI(_SET_TAG, "Audio source page already opened");
        return;
    }

    // 从 NVS 加载上次的协议选择，保持 UI 与 SpeakerApp::init 中读取的值一致
    ProtocolType protocol_type = ProtocolType::PROTOCOL_A2DP;
    NvsHelper::load("common_config", "audio_protocol",
                    reinterpret_cast<uint8_t *>(&protocol_type), sizeof(ProtocolType));
    if (protocol_type == ProtocolType::PROTOCOL_AIRPLAY) {
        _current_protocol = 1;
    } else {
        _current_protocol = 0;
    }

    _audio_source_page = lv_obj_create(lv_scr_act());
    configFullScreenPage(_audio_source_page, audioSourceGestureCb);
    buildPageHeader(_audio_source_page, "Audio Source", audioSourceBackBtnEventCb);

    // 协议选择提示语
    lv_obj_t *tip_label = lv_label_create(_audio_source_page);
    lv_label_set_text(tip_label, "Select streaming protocol:");
    lv_obj_set_style_text_color(tip_label, COLOR_SETTING_TEXT_COLOR, 0);
    lv_obj_align(tip_label, LV_ALIGN_TOP_LEFT, 20, 50);

    // 协议卡片纵向容器
    lv_obj_t *container = lv_obj_create(_audio_source_page);
    lv_obj_set_size(container, CONFIG_LCD_H_RES - 40, CONFIG_LCD_V_RES - 40);
    lv_obj_align(container, LV_ALIGN_TOP_MID, 0, 75);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_pad_row(container, 10, 0);
    lv_obj_set_layout(container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    // 选项 A: 蓝牙 (A2DP)
    _btn_bt = createProtocolCard(container, LV_SYMBOL_BLUETOOTH "  Bluetooth Audio",
                                 protocolSelectEventCb);

    // 选项 B: AirPlay
    _btn_airplay = createProtocolCard(container, LV_SYMBOL_WIFI "  Apple AirPlay",
                                      protocolSelectEventCb);

    updateProtocolView();
}

void UiSetting::updateProtocolView()
{
    if(_btn_bt != nullptr && _btn_airplay != nullptr) {
        lv_obj_set_style_bg_color(_btn_bt,
            _current_protocol == 0 ? COLOR_SETTING_BTN_SELECTED : COLOR_SETTING_BTN_UNSELECTED, 0);
        lv_obj_set_style_bg_color(_btn_airplay,
            _current_protocol == 1 ? COLOR_SETTING_BTN_SELECTED : COLOR_SETTING_BTN_UNSELECTED, 0);
    }
}

void UiSetting::settingItemEventCb(lv_event_t * e)
{
    ESP_LOGI(_SET_TAG, "Open audio source sub page");
    UiSetting::instance().openAudioSourcePage();
}

void UiSetting::protocolSelectEventCb(lv_event_t * e)
{
    lv_obj_t * clicked_obj = (lv_obj_t *)lv_event_get_target(e);
    UiSetting & thiz = UiSetting::instance();

    ProtocolType protocol_type = ProtocolType::PROTOCOL_A2DP;
    if(clicked_obj == thiz._btn_bt) {
        thiz._current_protocol = 0;
        protocol_type = ProtocolType::PROTOCOL_A2DP;
    } else if(clicked_obj == thiz._btn_airplay) {
        thiz._current_protocol = 1;
        protocol_type = ProtocolType::PROTOCOL_AIRPLAY;
    } else {
        return;
    }

    // 立即写入 NVS，下次启动 SpeakerApp::init 会按该协议初始化栈
    NvsHelper::save("common_config", "audio_protocol",
                    reinterpret_cast<const uint8_t *>(&protocol_type), sizeof(ProtocolType));

    thiz.updateProtocolView();
}

void UiSetting::audioSourceBackBtnEventCb(lv_event_t * e)
{
    ESP_LOGI(_SET_TAG, "Audio source back button clicked");
    UiSetting::instance().closeAudioSourcePage();
}

void UiSetting::audioSourceGestureCb(lv_event_t * e)
{
    lv_indev_t * indev = lv_indev_active();
    if(indev == nullptr) return;

    // 在协议选择子页上滑返回设置列表
    if(lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        ESP_LOGI(_SET_TAG, "Swipe up, back to settings list");
        UiSetting::instance().closeAudioSourcePage();
    }
}

void UiSetting::closeAudioSourcePage()
{
    if(_audio_source_page) {
        lv_obj_del(_audio_source_page);
        _audio_source_page = nullptr;
        _btn_bt = nullptr;
        _btn_airplay = nullptr;
    }
}

void UiSetting::settingBackBtnEventCb(lv_event_t * e)
{
    ESP_LOGI(_SET_TAG, "Back button clicked, close settings page");
    UiSetting::instance().closeSettingPage();
}

void UiSetting::settingGestureCb(lv_event_t * e)
{
    lv_indev_t * indev = lv_indev_active();
    if(indev == nullptr) return;

    // 在设置列表上滑返回主页面
    if(lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        ESP_LOGI(_SET_TAG, "Swipe up, close settings page");
        UiSetting::instance().closeSettingPage();
    }
}

void UiSetting::closeSettingPage()
{
    // 若存在子页先关闭，再关闭设置列表页
    closeAudioSourcePage();
    if(_setting_page) {
        lv_obj_del(_setting_page);
        _setting_page = nullptr;
    }
}
