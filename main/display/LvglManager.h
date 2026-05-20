#ifndef __LVGL_MANAGER_H__
#define __LVGL_MANAGER_H__

#include <functional>
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "lvgl.h"
#include "TouchMgr.h"
#include "LCDMgr.h"
#include "UiMusicPlayer.h"

class LvglManager {
public:
    using FlushCallback = std::function<void(const lv_area_t *area, lv_color_t *color_p)>;

    static LvglManager &instance();
    static bool lvgl_lock(int timeout_ms);
    static void lvgl_unlock();


    esp_err_t init(uint16_t width, uint16_t height);
    esp_err_t deinit();

    bool isInitialized() const { return _initialized; }

private:
    static LvglManager *s_instance;
    static constexpr const char *_LVMGR_TAG = "LvglManager";
    static SemaphoreHandle_t _lvgl_mux;

    static void tickTimer(void *arg);
    static void lvglTask(void *arg);

    LvglManager();
    ~LvglManager();

    LvglManager(const LvglManager &) = delete;
    LvglManager &operator=(const LvglManager &) = delete;

    bool _initialized;
    bool _running;
    uint16_t _lcd_width;
    uint16_t _lcd_height;
    esp_timer_handle_t _tick_timer;
    TaskHandle_t _task_handle = nullptr;

    TouchMgr  touchMgr;
    LCDMgr lcdMgr;

};

#endif // __LVGL_MANAGER_H__