#ifndef __WIFI_MGR_H__
#define __WIFI_MGR_H__

#include "esp_event.h"
#include "esp_netif.h"
#include <cstddef>
#include <string>

#define DHCP_HOSTNAME_MAX_LEN 31

class WifiMgr {
public:
    WifiMgr(const WifiMgr&) = delete;
    WifiMgr& operator=(const WifiMgr&) = delete;

    static WifiMgr& instance();
    static void eventHandler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data);
    void init();
    void deinit();

    // AP netif handle — valid after init() succeeds, nullptr before that.
    esp_netif_t* getApNetif() const { return _ap_netif; }


    // Connect / disconnect the STA interface. Safe to call before init() returns
    // (it'll queue the work via esp_wifi_set_config + esp_wifi_connect).
    esp_err_t connectSta(const char* ssid, const char* pass);
    esp_err_t disconnectSta();

    bool isStaConnected() const { return _sta_connected; }
    esp_netif_t* getStaNetif() const { return _sta_netif; }
    int8_t getStaRssi() const { return _sta_rssi; }

    void setHostName(const char* hostname);
    void enableApMode(void);

    std::string getMacAddrStr();

private:
    static constexpr const char *_TAG_WIFI = "WifiMgr";
    static constexpr const char *_NVS_NS    = "wifi";
    static constexpr const char *_NVS_SSID  = "sta_ssid";
    static constexpr const char *_NVS_PASS  = "sta_pass";
    static constexpr size_t       _SSID_MAX = 32;
    static constexpr size_t       _PASS_MAX = 64;

    WifiMgr();
    ~WifiMgr();

    esp_netif_t* _ap_netif = nullptr;
    esp_netif_t* _sta_netif = nullptr;
    bool _sta_connected = false;
    int8_t _sta_rssi = 0;
    int8_t _retry_count = 0;

    void applyStaConfig(const char* ssid, const char* pass);
    void sanitizeHostname(const char* src, char* dst, size_t dst_len);
};

#endif // __WIFI_MGR_H__
