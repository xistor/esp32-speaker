#include "esp_err.h"
#include "esp_log.h"
#include "LCDMgr.h"

LCDMgr::LCDMgr() {
}

LCDMgr::~LCDMgr() {

}

void LCDMgr::init(uint16_t width, uint16_t height) {
    _lcd_width = width;
    _lcd_height = height;

    // Initialize LCD hardware, set up display parameters, etc.
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = CONFIG_LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 0,
    };

    ESP_ERROR_CHECK(spi_bus_initialize((spi_host_device_t)CONFIG_LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = CONFIG_LCD_CS,
        .dc_gpio_num = CONFIG_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = CONFIG_LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = CONFIG_LCD_CMD_BITS,
        .lcd_param_bits = CONFIG_LCD_PARAM_BITS,
    };

    // Attach the LCD to the SPI bus
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)CONFIG_LCD_HOST,
                                             &io_config, &_io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = CONFIG_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = CONFIG_BITS_PER_PIXEL,
    };

    ESP_LOGI(_LCDMGR_TAG, "Install ST7789 panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(_io_handle, &panel_config, &_panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(_panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(_panel_handle, 0, 0));

    // user can flush pre-defined pattern to the screen before we turn on the
    // screen or backlight
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(_panel_handle, true));

    // this flips the display vertically
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(_panel_handle, true, true));

    // create a lvgl display
    _disp = lv_display_create(_lcd_width, _lcd_height);

    // Set up LVGL display driver, register flush callback, etc.
    // This function will be called by LvglManager to set up the display driver
    // and register the flush callback that will be called by LVGL when it needs

    uint32_t lines = CONFIG_LVGL_DRAW_BUF_LINES;
    if (lines == 0) {
        lines = 20;
    }

    // alloc draw buffers used by LVGL
    // it's recommended to choose the size of the draw buffer(s) to be at least
    // 1/10 screen sized
    size_t draw_buffer_sz;
    draw_buffer_sz = _lcd_width * lines * sizeof(lv_color16_t);

    void *buf1 = spi_bus_dma_memory_alloc((spi_host_device_t)CONFIG_LCD_HOST, draw_buffer_sz, 0);
    assert(buf1);
    void *buf2 = spi_bus_dma_memory_alloc((spi_host_device_t)CONFIG_LCD_HOST, draw_buffer_sz, 0);
    assert(buf2);

    // initialize LVGL draw buffers
    lv_display_set_buffers(_disp, buf1, buf2, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    // associate the mipi panel handle to the display
    lv_display_set_user_data(_disp, _panel_handle);
    // set color depth
    lv_display_set_color_format(_disp, LV_COLOR_FORMAT_RGB565);
    // set the callback which can copy the rendered image to an area of the

    // display
    lv_display_set_flush_cb(_disp, flushCallback);

    ESP_LOGI(_LCDMGR_TAG, "Register io panel event callback for LVGL flush ready notification");
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };

    /* Register done callback */
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(_io_handle, &cbs, _disp));
}

void LCDMgr::deinit() {
    if (!_disp) {
        return;
    }

    if (_disp) {
        lv_display_delete(_disp);
        _disp = nullptr;
    }

    if (_panel_handle) {
        esp_lcd_panel_del(_panel_handle);
        _panel_handle = nullptr;
    }

    if (_io_handle) {
        esp_lcd_panel_io_del(_io_handle);
        _io_handle = nullptr;
    }
}

void LCDMgr::flushCallback(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
    // because SPI LCD is big-endian, we need to swap the RGB bytes order
    lv_draw_sw_rgb565_swap(px_map, (offsetx2 + 1 - offsetx1) *
                                       (offsety2 + 1 - offsety1));
    // copy a buffer's content to a specific area of the display
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1,
                              offsety2 + 1, px_map);
}

bool LCDMgr::notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}