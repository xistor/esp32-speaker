#include "WifiMgr.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>

#include "Setting.h"

WifiMgr::WifiMgr()
{
}

WifiMgr::~WifiMgr()
{
}

WifiMgr& WifiMgr::instance()
{
    static WifiMgr instance;
    return instance;
}

void WifiMgr::eventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    auto& self = WifiMgr::instance();

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t* e = (wifi_event_sta_disconnected_t*) event_data;
            ESP_LOGW(_TAG_WIFI, "STA disconnected reason=%d", e->reason);
            self._sta_connected = false;
            // Auto-reconnect after a short delay to ride out transient AP outages.

            if (self._retry_count < 5) {
                self._retry_count++;
                ESP_LOGI(_TAG_WIFI, "Retrying STA connection (%d/5)...", self._retry_count);
                vTaskDelay(5000 / portTICK_PERIOD_MS); // 5s
                esp_wifi_connect();
            } else {
                ESP_LOGW(_TAG_WIFI, "STA connection failed after %d retries", self._retry_count);

                self.enableApMode();
            }
            break;
        }
        case WIFI_EVENT_STA_CONNECTED:
            self._sta_connected = true;
            self._retry_count = 0;

            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t* e = (wifi_event_ap_staconnected_t*) event_data;
            ESP_LOGI(_TAG_WIFI, "AP: station connected MAC=%02x:%02x:%02x:%02x:%02x:%02x AID=%d",
                     e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], e->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t* e = (wifi_event_ap_stadisconnected_t*) event_data;
            ESP_LOGW(_TAG_WIFI, "AP: station disconnected MAC=%02x:%02x:%02x:%02x:%02x:%02x reason=%d",
                     e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], e->reason);
            break;
        }
        default: break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(_TAG_WIFI, "STA got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        self._sta_connected = true;

        // Disable AP mode when STA connects
        wifi_mode_t mode;
        if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_APSTA) {
            ESP_LOGI(_TAG_WIFI, "STA connected, disabling AP mode");
            esp_wifi_set_mode(WIFI_MODE_STA);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        self._sta_connected = false;
    }
}

void WifiMgr::init()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_country_t country = {
        .cc = "CN",
        .schan = 1,
        .nchan = 13,
        .max_tx_power = 80,
        .policy = WIFI_COUNTRY_POLICY_AUTO,
    };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &eventHandler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &eventHandler, NULL, NULL));

    // Create netifs for both interfaces.
    _sta_netif = esp_netif_create_default_wifi_sta();
    assert(_sta_netif);
    _ap_netif = esp_netif_create_default_wifi_ap();
    assert(_ap_netif);

    // AP+STA so the web UI stays reachable while the device is also
    // connected upstream. Country code is set above so this is stable.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = CONFIG_WIFI_AP_SSID,
            .ssid_len = strlen(CONFIG_WIFI_AP_SSID),
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 5,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(_ap_netif));
    ESP_ERROR_CHECK(esp_wifi_start());

    setHostName(CONFIG_SPEAKER_DEVICE_NAME);

    // Auto-reconnect to the last saved STA network, if any.
    char ssid[_SSID_MAX + 1] = {0};
    char pass[_PASS_MAX + 1] = {0};
    if (Setting::instance().getWifiCredentials(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK) {
        ESP_LOGI(_TAG_WIFI, "Found saved STA credentials for '%s', connecting...", ssid);
        applyStaConfig(ssid, pass);
        esp_wifi_connect();
    } else
    {
        ESP_LOGI(_TAG_WIFI, "No saved STA credentials found, staying in AP mode");
    }
}

void WifiMgr::deinit()
{
    esp_wifi_stop();
    esp_wifi_deinit();
}

void WifiMgr::enableApMode(void) {
  wifi_mode_t mode;
  if (esp_wifi_get_mode(&mode) == ESP_OK && mode != WIFI_MODE_APSTA) {
    ESP_LOGI(_TAG_WIFI, "Re-enabling AP mode for configuration access");
    if (!_ap_netif) {
      _ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = CONFIG_WIFI_AP_SSID,
            .ssid_len = strlen(CONFIG_WIFI_AP_SSID),
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 5,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config);
  }
}


void WifiMgr::applyStaConfig(const char* ssid, const char* pass)
{
    wifi_config_t sta_cfg = {};
    // snprintf truncates the source safely and always NUL-terminates, so
    // gcc -Wstringop-truncation stays quiet even when ssid/pass are at
    // exactly the max length.
    snprintf((char*)sta_cfg.sta.ssid,     sizeof(sta_cfg.sta.ssid),     "%s", ssid);
    snprintf((char*)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), "%s", pass);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;   // accept open + secured
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
}

esp_err_t WifiMgr::connectSta(const char* ssid, const char* pass)
{
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;
    if (!pass) pass = "";

    esp_err_t err = Setting::instance().setWifiCredentials(ssid, pass);
    if (err != ESP_OK) return err;

    ESP_LOGI(_TAG_WIFI, "Connecting to STA network '%s'...", ssid);

    _sta_connected = false;
    applyStaConfig(ssid, pass);
    return esp_wifi_connect();
}

esp_err_t WifiMgr::disconnectSta()
{
    esp_err_t err = esp_wifi_disconnect();
    _sta_connected = false;
    return err;
}

// RFC 1123 hostname: letters, digits and hyphens only, no leading or trailing
// hyphen. Separators, symbols and non-ASCII bytes (spaces, '_', '.', UTF-8
// names) collapse into a single '-', and the result is lowercased since
// hostnames are case-insensitive.
void WifiMgr::sanitizeHostname(const char* src, char* dst, size_t dst_len)
{
    if (dst == nullptr || dst_len == 0) {
        return;
    }

    size_t out = 0;
    for (size_t i = 0; src != nullptr && src[i] != '\0' && (out + 1) < dst_len; ++i) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!alnum && (out == 0 || dst[out - 1] == '-')) {
            continue;   // no leading or consecutive '-'
        }
        dst[out++] = alnum ? c : '-';
    }
    while (out > 0 && dst[out - 1] == '-') {
        --out;          // '-' must not be the last character
    }
    if (out == 0) {
        // Nothing usable left (empty name, only symbols, CJK, ...): fall back
        // to a fixed name instead of leaving the interface unnamed.
        snprintf(dst, dst_len, "%s", "esp-speaker");
    } else {
        dst[out] = '\0';
    }
}

void  WifiMgr::setHostName(const char* hostname)
{
    if (!_sta_netif || !hostname) {
        return;
    }
    char hn[DHCP_HOSTNAME_MAX_LEN + 1];
    sanitizeHostname(hostname, hn, sizeof(hn));
    if (strcmp(hn, hostname) != 0) {
        ESP_LOGI(_TAG_WIFI, "Hostname '%s' sanitized to '%s'", hostname, hn);
    }
    esp_err_t err = esp_netif_set_hostname(_sta_netif, hn);
    if (err != ESP_OK) {
        ESP_LOGE(_TAG_WIFI, "Failed to set hostname '%s': %s", hn,
                esp_err_to_name(err));
    } else {
        ESP_LOGI(_TAG_WIFI, "Hostname set to: %s", hn);
    }
}

std::string WifiMgr::getMacAddrStr()
{
    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGE(_TAG_WIFI, "Failed to get MAC address: %s", esp_err_to_name(err));
        return "";
    }

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(mac_str);
}