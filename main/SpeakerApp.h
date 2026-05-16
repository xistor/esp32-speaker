#ifndef __SPEAKER_APP_H__
#define __SPEAKER_APP_H__

#include <memory>
#include <functional>

#include "AudioI2s.h"
#include "esp_a2dp_api.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_avrc_api.h"

#include "BlockingQueue.h"
#include "UiMusicPlayer.h"

#define APP_DELAY_VALUE 50  // 5ms

/**
 * 
 *
 * Call `init()` after constructing an instance.  Configuration of the
 * I2S peripheral happens via `configureI2s()` before the first
 * connection attempt.
 */
class SpeakerApp {
public:

    using BtAppCallback = std::function<void(uint16_t event, void *param)>;
    using BtAppCopyCallback = std::function<void(void *p_dest, void *p_src, int len)>;
    using DeepFreeCallback = std::function<void(void *ptr)>;

    typedef struct {
        uint16_t event;
        BtAppCallback callback;
        void *param;
        DeepFreeCallback free_callback;
    } bt_app_msg_t;

    SpeakerApp();
    ~SpeakerApp();

    /**
     * Initialize Bluetooth stack, register callbacks, etc.
     * Returns ESP_OK on success.
     */
    esp_err_t init();

    /**
     * Configure the I2S driver (can be called multiple times).
     */
    void configureI2s(const i2s_chan_config_t &chan_cfg,
                      const i2s_std_config_t &std_cfg);

private:
    static SpeakerApp *s_instance;
    static constexpr const char *_XSPK_TAG = "X_SPEAKER";

    // Bluetooth callbacks have to be static, so they route to the
    // singleton instance.
    static void a2dCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
    static void gapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
    static void a2dDataCallback(const uint8_t *data, uint32_t len);
    static void rcCtrlCallback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param); 
    static void rcTgCallback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);
    static void avrcCommonnCopyMetaData(void *p_dest, void *p_src, int len);
    static void avrcCommonFreeMetaData(void *ptr);
    static uint8_t allocTransactionLabel();

    void handleA2dpEvent(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
    void handleGapEvent(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
    void handleA2dpData(const uint8_t *data, uint32_t len);
    void handleRcCtrlEvent(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
    void handleRcTgEvent(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);

    // internal helpers
    void setScanModeConnectable(bool conn, bool discoverable);

    bool msgDispatch(BtAppCallback callback, uint16_t event, void *p_params, int param_len,
                    BtAppCopyCallback copy_callback = nullptr, DeepFreeCallback free_callback = nullptr);
    void msgHandler();
    void pushBtMsg(const bt_app_msg_t &msg);

    void saveCoverImageData(const uint8_t *data, uint32_t len);

    const char *_device_name = CONFIG_SPEAKER_DEVICE_NAME;
    AudioI2s _audio_i2s;
    UiMusicPlayer _ui_music_player;
    uint8_t _cover_image_handler[7];
    uint8_t *_cover_image_data = nullptr;
    uint32_t _cover_image_size = 0;
    uint16_t *_cover_pixels = nullptr;

    BlockingQueue <bt_app_msg_t> _bt_msg_queue;
    std::thread _msg_handler_thread;

};

#endif // __SPEAKER_APP_H__