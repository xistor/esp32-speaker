#ifndef __TOUCH_MGR_H__
#define __TOUCH_MGR_H__

#include <thread>
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"

#include "esp_lcd_touch_cst816s.h"

class TouchMgr {
public:
    TouchMgr();
    void init();
    void deinit();
    static void touchReadCallback(lv_indev_t * drv, lv_indev_data_t * data);

private:
    static TouchMgr *s_instance;
    static constexpr const char *_TAG_TOUCH = "TouchMgr";
    static void touchIntCallback(esp_lcd_touch_handle_t tp);

    SemaphoreHandle_t _touch_mux;
    std::mutex _touch_lock;
    i2c_master_bus_handle_t _i2c_handle = nullptr;
    esp_lcd_touch_handle_t _touch_handle = nullptr;
    std::thread _touch_read_task_thread;
    std::atomic<bool> _running{false};
    lv_indev_data_t _last_touch_data;


    void handleTouchInt(esp_lcd_touch_handle_t tp);
    void handleTouchRead(lv_indev_data_t * data);

    void touchReadTask();
    const lv_indev_data_t& getLastTouchData() const { return _last_touch_data; }
};

#endif // __TOUCH_MGR_H__