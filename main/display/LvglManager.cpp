#include <sys/param.h>
#include <unistd.h>
#include "esp_err.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "LvglManager.h"

static void play_pause_event_cb(lv_event_t * e) {
    lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t * label = lv_obj_get_child(btn, 0);

    ESP_LOGI("LVGL", "Play/Pause Button Clicked!");
    // Toggle between Play and Pause icons
    const char * txt = lv_label_get_text(label);
    if(strcmp(txt, LV_SYMBOL_PLAY) == 0) {
        lv_label_set_text(label, LV_SYMBOL_PAUSE);
    } else {
        lv_label_set_text(label, LV_SYMBOL_PLAY);
    }
}

SemaphoreHandle_t LvglManager::_lvgl_mux = nullptr;

LvglManager &LvglManager::instance()
{
    static LvglManager instance;
    return instance;
}

LvglManager::LvglManager()
    : _initialized(false), _running(false), _lcd_width(0), _lcd_height(0), _tick_timer(nullptr), _task_handle(nullptr)
{
}

LvglManager::~LvglManager()
{
    deinit();
}

esp_err_t LvglManager::init(uint16_t width, uint16_t height)
{

    if (_initialized) {
        ESP_LOGW(_LVMGR_TAG, "already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    _lvgl_mux = xSemaphoreCreateRecursiveMutex();

    lv_init();

    _lcdMgr.init(width, height);
    _touchMgr.init();

    // uiMusicPlayer.create_ui();

    ESP_LOGI(_LVMGR_TAG, "Install LVGL tick timer");
    const esp_timer_create_args_t timer_args = {
        .callback = &LvglManager::tickTimer,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lvgl_tick",
        .skip_unhandled_events = false
    };

    esp_err_t err = esp_timer_create(&timer_args, &_tick_timer);
    if (err != ESP_OK) {
        ESP_LOGE(_LVMGR_TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        return err;
    }

    const int tick_ms = CONFIG_LVGL_TICK_PERIOD_MS;
    err = esp_timer_start_periodic(_tick_timer, (uint64_t)tick_ms * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(_LVMGR_TAG, "esp_timer_start_periodic failed: %s", esp_err_to_name(err));
        return err;
    }

    _running = true;

    BaseType_t res = xTaskCreatePinnedToCore(&LvglManager::lvglTask, "lvgl_task", CONFIG_LVGL_TASK_STACK_SIZE, this,
                                            CONFIG_LVGL_TASK_PRIORITY, &_task_handle, 1);
    if (res != pdPASS) {
        ESP_LOGE(_LVMGR_TAG, "lvgl task create failed: %d", res);
        _running = false;
        return ESP_ERR_NO_MEM;
    }

    _initialized = true;
    ESP_LOGI(_LVMGR_TAG, "LVGL initialized %ux%u", width, height);

    return ESP_OK;
}

esp_err_t LvglManager::deinit()
{
    if (!_initialized) {
        return ESP_OK;
    }

    _touchMgr.deinit();

    _lcdMgr.deinit();

    _running = false;

    if (_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(20));
        vTaskDelete(_task_handle);
        _task_handle = nullptr;
    }

    if (_tick_timer) {
        esp_timer_stop(_tick_timer);
        esp_timer_delete(_tick_timer);
        _tick_timer = nullptr;
    }

    _initialized = false;
    ESP_LOGI(_LVMGR_TAG, "LVGL deinitialized");
    return ESP_OK;
}

bool  LvglManager::lvgl_lock(int timeout_ms)
{
    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(_lvgl_mux, timeout_ticks) == pdTRUE;
}

void LvglManager::lvgl_unlock()
{
    xSemaphoreGiveRecursive(_lvgl_mux);
}

void LvglManager::tickTimer(void *arg)
{
    (void)arg;
    lv_tick_inc(CONFIG_LVGL_TICK_PERIOD_MS);
}

void LvglManager::lvglTask(void *arg)
{
    ESP_LOGI(_LVMGR_TAG, "Starting LVGL task");

    LvglManager *manager = reinterpret_cast<LvglManager *>(arg);
    uint32_t time_till_next_ms = 0;

    while (manager && manager->_running) {
        
        if (lvgl_lock(-1)) {
            time_till_next_ms = lv_timer_handler();
            /* Release the mutex */
            lvgl_unlock();
        }

        // in case of triggering a task watch dog time out
        time_till_next_ms = MAX(time_till_next_ms, CONFIG_LVGL_TASK_MIN_DELAY_MS);
        // in case of lvgl display not ready yet
        time_till_next_ms = MIN(time_till_next_ms, CONFIG_LVGL_TASK_MAX_DELAY_MS);
        
        vTaskDelay(pdMS_TO_TICKS(time_till_next_ms));

    }

    ESP_LOGI(_LVMGR_TAG, "Exiting LVGL task");
    vTaskDelete(nullptr);
}