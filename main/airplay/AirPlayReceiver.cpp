#include "AirPlayReceiver.h"

#include <new>
#include <string>

#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "AirPlayCrypto.h"
#include "AirPlayMdns.h"
#include "AudioPipeline.h"
#include "RtspServer.h"
#include "Setting.h"
#include "UiMusicPlayer.h"
#include "WifiMgr.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace airplay {

namespace {
constexpr const char* TAG = "AirPlay";

void destroyAudioPipeline(AudioPipeline*& pipeline)
{
    if (pipeline == nullptr) {
        return;
    }
    delete pipeline;
    pipeline = nullptr;
}

// Artwork needs both the build-time capability and the runtime setting: the
// AirPlay decode path is only compiled in when CONFIG_AIRPLAY_ENABLE_ARTWORK
// is set, while the user can still turn covers off from the settings page.
constexpr bool kArtworkCapable =
#ifdef CONFIG_AIRPLAY_ENABLE_ARTWORK
    true;
#else
    false;
#endif

bool artworkEnabled()
{
    return kArtworkCapable && Setting::instance().getShowAlbumArt();
}

} // namespace

AirPlayReceiver::~AirPlayReceiver()
{
    stop();
    delete _server;
    _server = nullptr;
    destroyAudioPipeline(_audio);
}

esp_err_t AirPlayReceiver::init(AudioI2s* audioI2s)
{
    if (_audio != nullptr) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "heap at AirPlay init: internal=%u largest=%u psram=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    // RSA key parsing and the PSA self-test need more stack than the main
    // task has on this board. Run them on a dedicated short-lived thread.
    esp_err_t error = ESP_FAIL;
    esp_pthread_cfg_t cryptoCfg = esp_pthread_get_default_config();
    cryptoCfg.thread_name = "airplay_crypto";
    cryptoCfg.prio = 5;
    cryptoCfg.stack_size = 16384;
    cryptoCfg.pin_to_core = 0;
    cryptoCfg.inherit_cfg = false;
    esp_pthread_set_cfg(&cryptoCfg);
    try {
        std::thread cryptoThread([&error]() {
            error = AirPlayCrypto::instance().init();
        });
        cryptoThread.join();
    } catch (...) {
        ESP_LOGE(TAG, "Unable to create crypto init thread");
        return ESP_ERR_NO_MEM;
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Crypto init failed");
        return error;
    }

    try {
        _audio = new AudioPipeline();
    } catch (...) {
        return ESP_ERR_NO_MEM;
    }
    if (_audio == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    error = _audio->init(audioI2s);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Audio pipeline init failed: %s",
                 esp_err_to_name(error));
        return error;
    }

    try {
        _server = new RtspServer();
    } catch (...) {
        destroyAudioPipeline(_audio);
        return ESP_ERR_NO_MEM;
    }
    if (_server == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t AirPlayReceiver::start()
{
    if (_running.load()) {
        return ESP_OK;
    }
    if (_audio == nullptr || _server == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    _running.store(true);

    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.thread_name = "airplay_mon";
    cfg.prio = 4;
    cfg.stack_size = 3072;
    cfg.pin_to_core = 0;
    cfg.inherit_cfg = false;
    esp_pthread_set_cfg(&cfg);

    _monitorThread = std::thread(&AirPlayReceiver::monitorLoop, this);
    return ESP_OK;
}

void AirPlayReceiver::stop()
{
    bool wasRunning = _running.exchange(false);
    if (_monitorThread.joinable()) {
        _monitorThread.join();
    }
    stopServices();
    if (wasRunning) {
        ESP_LOGI(TAG, "AirPlay receiver stopped");
    }
}

bool AirPlayReceiver::isConnected()
{
    if (!_servicesStarted.load() || _server == nullptr) {
        return false;
    }
    return _server->hasSession();
}

bool AirPlayReceiver::sendRemoteCommand(RemoteCommand command)
{
    if (!_servicesStarted.load() || _server == nullptr) {
        return false;
    }
    return _server->sendRemoteCommand(command);
}

void AirPlayReceiver::setLocalMuted(bool muted)
{
    if (_servicesStarted.load() && _server != nullptr) {
        _server->setLocalMuted(muted);
    }
}

void AirPlayReceiver::monitorLoop()
{
    while (_running.load()) {
        bool connected = WifiMgr::instance().isStaConnected();
        uint32_t currentIp = 0;
        if (connected) {
            esp_netif_ip_info_t info = {};
            esp_netif_t* netif = WifiMgr::instance().getStaNetif();
            connected = netif != nullptr &&
                        esp_netif_get_ip_info(netif, &info) == ESP_OK &&
                        info.ip.addr != 0;
            if (connected) {
                currentIp = info.ip.addr;
            }
        }
        bool started = _servicesStarted.load();

        if (started) {
            // "md" is advertised from the build-time capability only, so the
            // runtime switch just stops local decoding. Clear the artwork
            // de-duplication cache when it is turned back on, otherwise a
            // cover re-sent for the current track is filtered out.
            const bool artwork = artworkEnabled();
            if (artwork && !_lastArtworkEnabled && _server != nullptr) {
                _server->resetArtworkCache();
                ESP_LOGI(TAG, "Album art enabled; artwork cache cleared");
            }
            _lastArtworkEnabled = artwork;
        }

        if (connected && started && _server != nullptr &&
            _audio != nullptr && _server->isRecording() &&
            _audio->isRtpStalled(10000)) {
            ESP_LOGW(TAG, "RTP stalled; restarting AirPlay services");
            stopServices();
        } else if (connected && started && _lastIp != 0 &&
                   currentIp != _lastIp) {
            ESP_LOGW(TAG, "Wi-Fi IP changed; restarting AirPlay services");
            stopServices();
        } else if (connected && !started) {
            if (!startServices()) {
                ESP_LOGW(TAG, "AirPlay services not started; will retry");
            }
        } else if (!connected && started) {
            ESP_LOGW(TAG, "Wi-Fi disconnected; stopping AirPlay services");
            stopServices();
        }
        _lastIp = connected ? currentIp : 0;

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

bool AirPlayReceiver::startServices()
{
    std::string mac = WifiMgr::instance().getMacAddrStr();
    if (mac.empty()) {
        ESP_LOGW(TAG, "No Wi-Fi MAC address yet");
        return false;
    }

    esp_netif_t* netif = WifiMgr::instance().getStaNetif();
    esp_netif_ip_info_t ipInfo = {};
    if (netif != nullptr &&
        esp_netif_get_ip_info(netif, &ipInfo) == ESP_OK) {
        ESP_LOGI(TAG, "STA network: ip=" IPSTR " netmask=" IPSTR
                      " gateway=" IPSTR,
                 IP2STR(&ipInfo.ip), IP2STR(&ipInfo.netmask),
                 IP2STR(&ipInfo.gw));
    }

    // AirPlay is latency-sensitive; modem sleep adds 100+ ms of jitter.
    esp_err_t psError = esp_wifi_set_ps(WIFI_PS_NONE);
    if (psError != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable Wi-Fi power save: %s",
                 esp_err_to_name(psError));
    }

    esp_err_t error = mdnsStart(CONFIG_SPEAKER_DEVICE_NAME, mac.c_str(),
                                kArtworkCapable);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "mDNS start failed: %s", esp_err_to_name(error));
        return false;
    }
    _lastArtworkEnabled = artworkEnabled();

    // The spectrum analyser is fed from the playback path, so it works in
    // AirPlay mode exactly like it does for Bluetooth.
    _audio->setVisualizer(&UiMusicPlayer::instance());

    if (!_server->start(_audio, &UiMusicPlayer::instance())) {
        ESP_LOGE(TAG, "RTSP server start failed");
        mdnsStop();
        return false;
    }

    _servicesStarted.store(true);
    ESP_LOGI(TAG, "AirPlay services started");
    return true;
}

void AirPlayReceiver::stopServices()
{
    if (!_servicesStarted.exchange(false)) {
        return;
    }
    if (_server != nullptr) {
        _server->stop();
    }
    mdnsStop();
    ESP_LOGI(TAG, "AirPlay services stopped");
}

} // namespace airplay
