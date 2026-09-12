#include "UiSetting.h"
#include "LvglManager.h"
#include "esp_log.h"
#include "colorDef.h"

void UiSetting::createSettingPage() {

    _setting_page = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_setting_page, CONFIG_LCD_H_RES, CONFIG_LCD_V_RES);
    lv_obj_set_style_bg_color(_setting_page, COLOR_BACKGROUND, 0);
    lv_obj_set_style_border_width(_setting_page, 0, 0);
    lv_obj_set_style_radius(_setting_page, 0, 0);
    lv_obj_remove_flag(_setting_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(_setting_page);

    lv_obj_t * header = lv_obj_create(_setting_page);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, CONFIG_LCD_H_RES, 40);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t * btn_back = lv_btn_create(header);
    lv_obj_set_size(btn_back, 35, 35);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(btn_back, 0, 0);
    lv_obj_t * label_back = lv_label_create(btn_back);
    lv_label_set_text(label_back, LV_SYMBOL_LEFT); 
    lv_obj_set_style_text_color(label_back, lv_color_white(), 0);
    lv_obj_center(label_back);
    lv_obj_add_event_cb(btn_back, settingBackBtnEventCb, LV_EVENT_CLICKED, this);

    // 2.2 页面标题
    lv_obj_t * title = lv_label_create(header);
    lv_label_set_text(title, "Audio Source");
    lv_obj_set_style_text_color(title, COLOR_SETTING_TITLE_COLOR, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    // 3. 协议选择区域主提示语
    lv_obj_t * tip_label = lv_label_create(_setting_page);
    lv_label_set_text(tip_label, "Select streaming protocol:");
    lv_obj_set_style_text_color(tip_label, COLOR_SETTING_TEXT_COLOR, 0);
    lv_obj_align(tip_label, LV_ALIGN_TOP_LEFT, 20, 50);

    // 4. 创建纵向排列的两个大选项卡片 (Flex 容器)
    lv_obj_t * container = lv_obj_create(_setting_page);
    lv_obj_set_size(container, CONFIG_LCD_H_RES - 40, CONFIG_LCD_V_RES - 40);
    lv_obj_align(container, LV_ALIGN_TOP_MID, 0, 75);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_layout(container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // 选项 A: 蓝牙 Bluetooth
    lv_obj_t *_btn_bt = lv_obj_create(container);
    lv_obj_set_width(_btn_bt, lv_pct(100));
    lv_obj_set_style_pad_all(_btn_bt, 0, 0);
    lv_obj_set_height(_btn_bt, 50);
    lv_obj_set_style_border_width(_btn_bt, 0, 0);
    lv_obj_set_style_radius(_btn_bt, 10, 0);
    lv_obj_add_flag(_btn_bt, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_btn_bt, protocolSelectEventCb, LV_EVENT_CLICKED, this);

    lv_obj_t * lbl_bt = lv_label_create(_btn_bt);
    lv_label_set_text(lbl_bt, LV_SYMBOL_BLUETOOTH "  Bluetooth Audio");
    lv_obj_set_style_text_color(lbl_bt, lv_color_white(), 0);
    lv_obj_align(lbl_bt, LV_ALIGN_LEFT_MID, 15, 0);

    // 选项 B: AirPlay
    lv_obj_t *_btn_airplay = lv_obj_create(container);
    lv_obj_set_width(_btn_airplay, lv_pct(100));
    lv_obj_set_style_pad_all(_btn_airplay, 0, 0);
    lv_obj_set_height(_btn_airplay, 50);
    lv_obj_set_style_border_width(_btn_airplay, 0, 0);
    lv_obj_set_style_radius(_btn_airplay, 10, 0);
    lv_obj_add_flag(_btn_airplay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_btn_airplay, protocolSelectEventCb, LV_EVENT_CLICKED, this);

    lv_obj_t * lbl_ap = lv_label_create(_btn_airplay);
    lv_label_set_text(lbl_ap, LV_SYMBOL_WIFI "  Apple AirPlay");
    lv_obj_set_style_text_color(lbl_ap, lv_color_white(), 0);
    lv_obj_align(lbl_ap, LV_ALIGN_LEFT_MID, 15, 0);

    if (_current_protocol == 0) {
        lv_obj_set_style_bg_color(_btn_bt, COLOR_SETTING_BTN_SELECTED, 0);
        lv_obj_set_style_bg_color(_btn_airplay, COLOR_SETTING_BTN_UNSELECTED, 0);
    } else {
        lv_obj_set_style_bg_color(_btn_bt, COLOR_SETTING_BTN_UNSELECTED, 0);
        lv_obj_set_style_bg_color(_btn_airplay, COLOR_SETTING_BTN_SELECTED, 0);
    }


}

void UiSetting::protocolSelectEventCb(lv_event_t * e)
{
    lv_obj_t * clicked_obj = (lv_obj_t *)lv_event_get_target(e);
    UiSetting * thiz = (UiSetting *)lv_event_get_user_data(e);


}

void UiSetting::settingBackBtnEventCb(lv_event_t * e)
{
    ESP_LOGI(_SET_TAG, "Back button clicked!");
    // Handle the back button click event here

    lv_obj_del(UiSetting::instance()._setting_page);

}