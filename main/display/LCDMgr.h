#ifndef __LCD_MGR_H__
#define __LCD_MGR_H__

#include <functional>
#include <cstdint>

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_types.h"

#include "lvgl.h"

class LCDMgr {
public:

    using FlushCallback = std::function<void(const lv_area_t *area, lv_color_t *color_p)>;

    LCDMgr();
    ~LCDMgr();
    void init(uint16_t width, uint16_t height);
    void deinit();
    void setLCDBackLight(bool on);

private:
    static constexpr const char *_LCDMGR_TAG = "LCDMgr";
    uint16_t _lcd_width = 0;
    uint16_t _lcd_height = 0;

    lv_display_t *_disp = nullptr;
    FlushCallback _flush_cb = nullptr;
    esp_lcd_panel_handle_t _panel_handle = nullptr;
    esp_lcd_panel_io_handle_t _io_handle = nullptr;

    static void flushCallback(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
    static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                        esp_lcd_panel_io_event_data_t *edata,
                                        void *user_ctx);
};

#endif // __LCD_MGR_H__