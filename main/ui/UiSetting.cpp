#include "UiSetting.h"
#include "UiMusicPlayer.h"
#include "LvglManager.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "colorDef.h"
#include "Setting.h"
#include "SpeakerApp.h"

namespace {
/* 设置列表页每一行的标识，通过 LVGL 事件 user_data 传给 settingItemEventCb */
enum class SettingRow : uintptr_t {
    AUDIO_SOURCE = 0,
    ALBUM_ART = 1,
};
}

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
                                     lv_event_cb_t click_cb){
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

/* 设置列表中的一行：左侧图标 + 名称，右侧留空给箭头/开关 */
static lv_obj_t * createSettingRow(lv_obj_t *parent, const char *symbol,
                                   const char *text, lv_event_cb_t click_cb,
                                   void *user_data)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 52);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_bg_color(row, COLOR_SETTING_BTN_UNSELECTED, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, click_cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 15, 0);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, text);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 45, 0);

    return row;
}

/*
 * The Bluetooth and AirPlay stacks are brought up once in SpeakerApp::init
 * according to the stored source, so a new selection only takes effect after
 * a restart. Do it automatically instead of asking the user to power-cycle,
 * but wait long enough for LVGL to paint the "restarting" hint.
 */
static void scheduleRestart()
{
    esp_timer_handle_t timer = nullptr;
    esp_timer_create_args_t args = {};
    args.callback = [](void *) { esp_restart(); };
    args.name = "source_restart";
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        if (esp_timer_start_once(timer, 1500 * 1000) == ESP_OK) {
            return;
        }
        esp_timer_delete(timer);
    }

    ESP_LOGW("UI_SETTING", "Restart timer unavailable, restarting now");
    esp_restart();
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
    lv_obj_t *row = createSettingRow(list, LV_SYMBOL_AUDIO, "Audio Source",
                                     settingItemEventCb,
                                     reinterpret_cast<void *>(
                                         static_cast<uintptr_t>(SettingRow::AUDIO_SOURCE)));

    // 右侧：进入箭头（当前协议在子页中查看与选择）
    lv_obj_t *arrow = lv_label_create(row);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, COLOR_SETTING_TEXT_COLOR, 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -12, 0);

    // ---- 设置项：Show Album Art（是否显示专辑封面），右侧开关 ----
    // 封面对蓝牙和 AirPlay 同时生效：关闭后封面区域始终显示内置默认图片，
    // 也不会再向发送端请求/接收封面图。
    lv_obj_t *art_row = createSettingRow(list, LV_SYMBOL_IMAGE, "Album Art",
                                         settingItemEventCb,
                                         reinterpret_cast<void *>(
                                             static_cast<uintptr_t>(SettingRow::ALBUM_ART)));

    _album_art_switch = lv_switch_create(art_row);
    lv_obj_set_size(_album_art_switch, 44, 24);
    lv_obj_align(_album_art_switch, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_bg_color(_album_art_switch, lv_color_hex(0x555555), LV_PART_MAIN);
    const lv_style_selector_t checkedIndicator =
        static_cast<lv_style_selector_t>(LV_PART_INDICATOR) |
        static_cast<lv_style_selector_t>(LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(_album_art_switch, COLOR_SETTING_BTN_SELECTED,
                              checkedIndicator);
    lv_obj_add_event_cb(_album_art_switch, albumArtSwitchEventCb,
                        LV_EVENT_VALUE_CHANGED, nullptr);

    updateAlbumArtView();
}

void UiSetting::openAudioSourcePage()
{
    if(_audio_source_page != nullptr) {
        ESP_LOGI(_SET_TAG, "Audio source page already opened");
        return;
    }

    // 从 NVS 加载上次的协议选择，保持 UI 与 SpeakerApp::init 中读取的值一致
    _current_audio_source = Setting::instance().getAudioSource();

    _audio_source_page = lv_obj_create(lv_scr_act());
    configFullScreenPage(_audio_source_page, audioSourceGestureCb);
    buildPageHeader(_audio_source_page, "Audio Source", audioSourceBackBtnEventCb);

    // 协议选择提示语：切换后会自动重启，提示语提前告知用户
    _source_tip_label = lv_label_create(_audio_source_page);
    lv_label_set_text(_source_tip_label,
                      "Select streaming protocol:\nDevice restarts after switching.");
    lv_obj_set_style_text_color(_source_tip_label, COLOR_SETTING_TEXT_COLOR, 0);
    lv_obj_align(_source_tip_label, LV_ALIGN_TOP_LEFT, 20, 46);

    // 协议卡片纵向容器
    lv_obj_t *container = lv_obj_create(_audio_source_page);
    lv_obj_set_size(container, CONFIG_LCD_H_RES - 40, CONFIG_LCD_V_RES - 115);
    lv_obj_align(container, LV_ALIGN_TOP_MID, 0, 95);
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
            _current_audio_source == AudioSource::A2DP ? COLOR_SETTING_BTN_SELECTED : COLOR_SETTING_BTN_UNSELECTED, 0);
        lv_obj_set_style_bg_color(_btn_airplay,
            _current_audio_source == AudioSource::AIRPLAY ? COLOR_SETTING_BTN_SELECTED : COLOR_SETTING_BTN_UNSELECTED, 0);
    }
}

void UiSetting::settingItemEventCb(lv_event_t * e)
{
    const SettingRow row = static_cast<SettingRow>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    if(row == SettingRow::ALBUM_ART) {
        // 点击整行等同于点开关
        UiSetting::instance().toggleAlbumArt();
        return;
    }

    ESP_LOGI(_SET_TAG, "Open audio source sub page");
    UiSetting::instance().openAudioSourcePage();
}

void UiSetting::albumArtSwitchEventCb(lv_event_t * e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    const bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);

    Setting::instance().setShowAlbumArt(enabled);
    // 立即生效，无需重启；播放页封面区域会切回默认图片
    UiMusicPlayer::instance().setAlbumArtEnabled(enabled);
    SpeakerApp::instance().applyAlbumArtSetting(enabled);
    UiSetting::instance().updateAlbumArtView();

    ESP_LOGI(_SET_TAG, "Album art %s", enabled ? "enabled" : "disabled");
}

void UiSetting::updateAlbumArtView()
{
    if(_album_art_switch == nullptr) {
        return;
    }

    if(Setting::instance().getShowAlbumArt()) {
        lv_obj_add_state(_album_art_switch, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(_album_art_switch, LV_STATE_CHECKED);
    }
}

void UiSetting::toggleAlbumArt()
{
    const bool enabled = !Setting::instance().getShowAlbumArt();

    Setting::instance().setShowAlbumArt(enabled);
    UiMusicPlayer::instance().setAlbumArtEnabled(enabled);
    SpeakerApp::instance().applyAlbumArtSetting(enabled);
    updateAlbumArtView();

    ESP_LOGI(_SET_TAG, "Album art %s", enabled ? "enabled" : "disabled");
}

void UiSetting::protocolSelectEventCb(lv_event_t * e)
{
    lv_obj_t * clicked_obj = (lv_obj_t *)lv_event_get_target(e);
    UiSetting & thiz = UiSetting::instance();

    if(thiz._restarting) {
        return;
    }

    AudioSource selected;
    if(clicked_obj == thiz._btn_bt) {
        selected = AudioSource::A2DP;
    } else if(clicked_obj == thiz._btn_airplay) {
        selected = AudioSource::AIRPLAY;
    } else {
        return;
    }

    // 点中的就是当前协议时不必重启，避免误触导致正在播放时重启
    if(selected == thiz._current_audio_source) {
        thiz.updateProtocolView();
        return;
    }

    thiz._current_audio_source = selected;
    // 立即写入 NVS，重启后 SpeakerApp::init 会按该协议初始化栈
    Setting::instance().setAudioSource(selected);
    thiz.updateProtocolView();

    ESP_LOGI(_SET_TAG, "Audio source switched to %s, restarting",
             selected == AudioSource::AIRPLAY ? "AirPlay" : "Bluetooth");

    thiz._restarting = true;
    showRestartOverlay(selected);
    scheduleRestart();
}

void UiSetting::audioSourceBackBtnEventCb(lv_event_t * e)
{
    ESP_LOGI(_SET_TAG, "Audio source back button clicked");
    UiSetting::instance().closeAudioSourcePage();
}

/* 切换协议后需要重启才能生效。用整屏遮罩明确告知用户正在重启，
 * 同时拦截触摸，避免重启前继续点设置项。 */
void UiSetting::showRestartOverlay(AudioSource source)
{
    lv_obj_t *overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, CONFIG_LCD_H_RES, CONFIG_LCD_V_RES);
    lv_obj_center(overlay);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(overlay);

    lv_obj_t *spinner = lv_spinner_create(overlay);
    lv_obj_set_size(spinner, 60, 60);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -40);
    lv_spinner_set_anim_params(spinner, 900, 90);
    lv_obj_set_style_arc_width(spinner, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x00A2FF), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(spinner, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x00A2FF), LV_PART_INDICATOR);

    lv_obj_t *title = lv_label_create(overlay);
    lv_label_set_text(title, "Restarting...");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 30);

    lv_obj_t *subtitle = lv_label_create(overlay);
    lv_label_set_text(subtitle,
                      source == AudioSource::AIRPLAY ? "Switching to AirPlay"
                                                     : "Switching to Bluetooth");
    lv_obj_set_style_text_color(subtitle, COLOR_SETTING_TEXT_COLOR, 0);
    lv_obj_align(subtitle, LV_ALIGN_CENTER, 0, 55);
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
        _source_tip_label = nullptr;
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
        _album_art_switch = nullptr;
    }
}
