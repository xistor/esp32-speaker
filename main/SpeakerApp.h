#ifndef __SPEAKER_APP_H__
#define __SPEAKER_APP_H__

#include <memory>
#include <functional>

#include "esp_a2dp_api.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_avrc_api.h"
#include "esp_spp_api.h"

#include "BlockingQueue.h"
#include "UiMusicPlayer.h"

#include "AudioI2s.h"
#include "AudioDecoder.h"

#include "ThreadPool.h"


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
    SpeakerApp(const SpeakerApp&) = delete;
    SpeakerApp& operator=(const SpeakerApp&) = delete;

    using BtAppCallback = std::function<void(uint16_t event, void *param)>;
    using BtAppCopyCallback = std::function<void(void *p_dest, void *p_src, int len)>;
    using DeepFreeCallback = std::function<void(void *ptr)>;

    typedef struct {
        uint16_t event;
        BtAppCallback callback;
        void *param;
        DeepFreeCallback free_callback;
    } bt_app_msg_t;


    static SpeakerApp& instance() {
        static SpeakerApp instance;
        return instance;

    }

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

    static constexpr const char *_XSPK_TAG = "X_SPEAKER";

    // Bluetooth callbacks have to be static, so they route to the
    // singleton instance.
    static void a2dCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
    static void gapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
    static void sppCallback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param);
    static void a2dDataCallback(const uint8_t *data, uint32_t len);
    static void a2dAudioDataCallback(esp_a2d_conn_hdl_t conn_hdl, esp_a2d_audio_buff_t *audio_buf);
    static void rcCtrlCallback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param); 
    static void rcTgCallback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);
    static void avrcCommonnCopyMetaData(void *p_dest, void *p_src, int len);
    static void avrcCommonFreeMetaData(void *ptr);
    static uint8_t allocTransactionLabel();

    SpeakerApp();
    ~SpeakerApp();

    void registerA2dpSinkSeps(void);

    void handleA2dpEvent(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
    void handleGapEvent(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
    void handleSppEvent(esp_spp_cb_event_t event, esp_spp_cb_param_t *param);
    void handleA2dpData(const uint8_t *data, uint32_t len);
    void handleRcCtrlEvent(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
    void handleRcTgEvent(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);

    // play control
    void playControlCb(UiMusicPlayer::play_ctrl_param_t ctrl_param);
    void handlePlayControl(uint16_t event, UiMusicPlayer::play_ctrl_param_t *param);
    void mute(time_t duration_ms);
    void mute();
    void unmute();

    // internal helpers
    void setScanModeConnectable(bool conn, bool discoverable);

    bool msgDispatch(BtAppCallback callback, uint16_t event, void *p_params, int param_len,
                    BtAppCopyCallback copy_callback = nullptr, DeepFreeCallback free_callback = nullptr);
    void msgHandler();
    void pushBtMsg(const bt_app_msg_t &msg);

    void saveCoverImageData(const uint8_t *data, uint32_t len);

    void checkAndConnectBondedDevice();

    void a2dInit();
    void a2dDeinit();

    esp_err_t bluetoothInit();
    void bluetoothDeinit();

    void saveToNvs(const char *ns, const char *key, const uint8_t *data, size_t len);
    bool getFromNvs(const char *ns, const char *key, uint8_t *data, size_t len);

    const char *_device_name = CONFIG_SPEAKER_DEVICE_NAME;
    AudioI2s _audio_i2s;
#ifdef CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
    AudioDecoder _audio_decoder;
#endif
    UiMusicPlayer& _ui_music_player = UiMusicPlayer::instance();
    uint8_t _cover_image_handler[7];
    uint8_t *_cover_image_data = nullptr;
    uint32_t _cover_image_size = 0;
    uint16_t *_cover_pixels = nullptr;

    BlockingQueue <bt_app_msg_t> _bt_msg_queue;
    std::thread _msg_handler_thread;

    uint8_t _remote_scn = 0;
    esp_bd_addr_t _saved_peer_addr = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    const esp_spp_sec_t _sec_mask = ESP_SPP_SEC_AUTHENTICATE;
    const esp_spp_role_t _role_master = ESP_SPP_ROLE_MASTER;

    ThreadPool _worker_pool{2};


};

#endif // __SPEAKER_APP_H__