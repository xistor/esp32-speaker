#include "TouchMgr.h"
#include "esp_log.h"
#include "esp_pthread.h"

static const char *TAG = "TouchMgr";

TouchMgr *TouchMgr::s_instance = nullptr;

TouchMgr::TouchMgr() {
    _touch_mux = xSemaphoreCreateBinary();
    assert(_touch_mux != nullptr);

    s_instance = this;
}

void TouchMgr::init() {

    /* Initilize I2C */
    i2c_master_bus_config_t i2c_config = {
        .i2c_port = (gpio_num_t)CONFIG_TOUCH_I2C_NUM,
        .sda_io_num = (gpio_num_t)CONFIG_TOUCH_I2C_SDA,
        .scl_io_num = (gpio_num_t)CONFIG_TOUCH_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };

    i2c_new_master_bus(&i2c_config, &_i2c_handle);

    /* Initialize touch HW */
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = CONFIG_LCD_H_RES,
        .y_max = CONFIG_LCD_V_RES,
        .rst_gpio_num = (gpio_num_t)CONFIG_TOUCH_GPIO_RESET,
        .int_gpio_num = (gpio_num_t)CONFIG_TOUCH_GPIO_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },

        .flags = {
            .swap_xy = 0,
            .mirror_x = 1,
            .mirror_y = 0,
        },
        .interrupt_callback = touchIntCallback,
    };

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS,
        .on_color_trans_done = 0,
        .user_ctx = 0,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 0,
        .flags =
        {
            .dc_low_on_data = 0,
            .disable_control_phase = 1,
        },
        .scl_speed_hz = 100000,
    };

    tp_io_config.scl_speed_hz = CONFIG_TOUCH_I2C_CLK_HZ;

    esp_lcd_new_panel_io_i2c(_i2c_handle, &tp_io_config, &tp_io_handle);
    esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, &_touch_handle);

    esp_lcd_touch_set_mirror_y(_touch_handle, true); // 上下反转

    lv_indev_t * indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, TouchMgr::touchReadCallback);
    lv_indev_set_user_data(indev, _touch_handle);
    lv_timer_set_period(lv_indev_get_read_timer(indev), 100);

    ESP_LOGI(_TAG_TOUCH, "TouchMgr initialized");
}

void TouchMgr::deinit() {

    _running.store(false);
    if (_touch_read_task_thread.joinable()) {
        _touch_read_task_thread.join();
    }

    if (_touch_handle) {
        esp_lcd_touch_del(_touch_handle);
        _touch_handle = nullptr;
    }

    if (_i2c_handle) {
        i2c_del_master_bus(_i2c_handle);
        _i2c_handle = nullptr;
    }

    if (_touch_mux) {
        vSemaphoreDelete(_touch_mux);
        _touch_mux = nullptr;
    }
}


void TouchMgr::touchIntCallback(esp_lcd_touch_handle_t tp) {
    if (s_instance) {
        s_instance->handleTouchInt(tp);
    }
}

void TouchMgr::handleTouchInt(esp_lcd_touch_handle_t tp) {

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(_touch_mux, &xHigherPriorityTaskWoken);

    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

void TouchMgr::touchReadCallback(lv_indev_t * drv, lv_indev_data_t * data) {
    if (s_instance) {
        s_instance->handleTouchRead(data);
    }
    // ESP_LOGI(TAG, "Touch read: x=%d, y=%d, state=%s", data->point.x, data->point.y,
    //          (data->state == LV_INDEV_STATE_PRESSED) ? "PRESSED" : "RELEASED");
}

void TouchMgr::handleTouchRead(lv_indev_data_t * data) {
    uint16_t touchpad_x[1]  = {0};
    uint16_t touchpad_y[1]  = {0};
    uint8_t touchpad_cnt    = 0;

    /* Read touch controller data */
    if (xSemaphoreTake(_touch_mux, 0) == pdTRUE) {
        esp_lcd_touch_read_data(_touch_handle);
        xSemaphoreGive(_touch_mux);
    }

    /* Get coordinates */
    bool touchpad_pressed = esp_lcd_touch_get_coordinates(_touch_handle, touchpad_x, touchpad_y, NULL, &touchpad_cnt, 1);

    if (touchpad_pressed && touchpad_cnt > 0) {
        data->point.x = touchpad_x[0];
        data->point.y = touchpad_y[0];
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}