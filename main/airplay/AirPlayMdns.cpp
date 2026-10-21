#include "AirPlayMdns.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "AirPlayTypes.h"
#include "esp_log.h"
#include "mdns.h"

namespace airplay {

namespace {

constexpr const char* TAG = "AirPlayMdns";
bool s_started = false;
std::mutex s_mutex;

bool equalsIgnoreCase(const char* left, const char* right)
{
    if (left == nullptr || right == nullptr) {
        return false;
    }
    while (*left != '\0' && *right != '\0') {
        if (std::tolower(static_cast<unsigned char>(*left)) !=
            std::tolower(static_cast<unsigned char>(*right))) {
            return false;
        }
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

// Direct SRV lookup uses the unicast-response (QU) bit, so it can still
// succeed when multicast responses are filtered by the network.
bool queryDacpSrvLocked(const char* instance, uint32_t senderIp,
                        uint32_t& hostIp, uint16_t& port)
{
    constexpr int kAttempts = 1;
    constexpr uint32_t kTimeoutMs = 3000;

    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        mdns_result_t* result = nullptr;
        esp_err_t error = mdns_query_srv(instance, "_dacp", "_tcp",
                                         kTimeoutMs, &result);
        if (error != ESP_OK || result == nullptr || result->port == 0) {
            if (error != ESP_OK) {
                ESP_LOGW(TAG, "DACP SRV '%s' attempt %d/%d error: %s",
                         instance, attempt + 1, kAttempts,
                         esp_err_to_name(error));
            } else {
                ESP_LOGI(TAG, "DACP SRV '%s' attempt %d/%d no response",
                         instance, attempt + 1, kAttempts);
            }
            if (result != nullptr) {
                mdns_query_results_free(result);
            }
            continue;
        }

        uint16_t foundPort = result->port;
        uint32_t ip = senderIp;
        if (ip == 0) {
            for (mdns_ip_addr_t* address = result->addr; address != nullptr;
                 address = address->next) {
                if (address->addr.type == ESP_IPADDR_TYPE_V4) {
                    ip = address->addr.u_addr.ip4.addr;
                    break;
                }
            }
            if (ip == 0 && result->hostname != nullptr) {
                esp_ip4_addr_t address = {};
                if (mdns_query_a(result->hostname, kTimeoutMs, &address) ==
                    ESP_OK) {
                    ip = address.addr;
                }
            }
        }

        mdns_query_results_free(result);

        if (ip == 0) {
            ESP_LOGW(TAG, "DACP SRV '%s' has no usable IPv4 address",
                     instance);
            continue;
        }

        hostIp = ip;
        port = foundPort;
        ESP_LOGI(TAG, "DACP SRV '%s' -> %u.%u.%u.%u:%u", instance,
                 static_cast<unsigned>(ip & 0xff),
                 static_cast<unsigned>((ip >> 8) & 0xff),
                 static_cast<unsigned>((ip >> 16) & 0xff),
                 static_cast<unsigned>((ip >> 24) & 0xff),
                 static_cast<unsigned>(foundPort));
        return true;
    }

    return false;
}

std::string sanitizeHostname(const char* name)
{
    std::string result;
    if (name == nullptr) {
        return "esp32speaker";
    }
    for (const char* p = name; *p != '\0'; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (std::isalnum(c) || c == '-') {
            result.push_back(static_cast<char>(c));
        } else if (c == ' ' || c == '_') {
            result.push_back('-');
        }
    }
    if (result.empty()) {
        result = "esp32speaker";
    }
    return result;
}

} // namespace

esp_err_t mdnsStart(const char* deviceName, const char* mac,
                    bool artworkCapable)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_started) {
        return ESP_OK;
    }
    if (deviceName == nullptr || mac == nullptr || *mac == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t error = mdns_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(error));
        return error;
    }

    std::string hostname = sanitizeHostname(deviceName);
    error = mdns_hostname_set(hostname.c_str());
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(error));
        return error;
    }

    error = mdns_instance_name_set(deviceName);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "mdns_instance_name_set failed: %s",
                 esp_err_to_name(error));
        return error;
    }

    char upperMac[18] = {};
    size_t macLen = strlen(mac);
    if (macLen > 12) {
        macLen = 12;
    }
    for (size_t i = 0; i < macLen; ++i) {
        upperMac[i] = static_cast<char>(
            std::toupper(static_cast<unsigned char>(mac[i])));
    }

    char instance[96];
    snprintf(instance, sizeof(instance), "%s@%s", upperMac, deviceName);

    // AirPlay 1 / RAOP TXT record. Keep this deliberately small: every
    // capability advertised here has to be implemented by the receiver.
    mdns_txt_item_t txt[] = {
        {"txtvers", "1"},
        {"deviceid", mac},
        {"ch", "2"},
        {"cn", "1"},         // ALAC
        {"et", "0,1"},       // no encryption, RSA
        {"ek", "1"},         // RSA-encrypted AES key in SDP
        {"sv", "false"},
        {"da", "true"},
        {"sr", "44100"},
        {"ss", "16"},
        {"pw", "false"},
        {"vn", "3"},
        {"tp", "UDP"},
        // Advertised from the build-time capability, not from the runtime
        // display setting: senders only read this when they connect, so
        // changing it later would force a re-connect before artwork resumes.
        {"md", artworkCapable ? "0,1,2" : "0,2"}, // text, (artwork), progress
        {"sf", "0x4"},       // audio receiver, no active session
        {"am", "AudioAccessory5,1"},
    };

    error = mdns_service_add(instance, "_raop", "_tcp", kRtspPort, txt,
                             sizeof(txt) / sizeof(txt[0]));
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "mdns_service_add(_raop) failed: %s",
                 esp_err_to_name(error));
        return error;
    }

    s_started = true;
    ESP_LOGI(TAG, "Advertising RAOP as '%s' (%s)", instance, hostname.c_str());
    return ESP_OK;
}

void mdnsStop()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_started) {
        return;
    }
    mdns_service_remove("_raop", "_tcp");
    mdns_free();
    s_started = false;
    ESP_LOGI(TAG, "mDNS stopped");
}

void mdnsSetSessionActive(bool active)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_started) {
        return;
    }
    const char* value = active ? "0x20004" : "0x4";
    mdns_service_txt_item_set("_raop", "_tcp", "sf", value);
}

bool mdnsFindDacp(const char* dacpId, uint32_t senderIp,
                  uint32_t& hostIp, uint16_t& port)
{
    hostIp = 0;
    port = 0;
    if (dacpId == nullptr || *dacpId == '\0') {
        return false;
    }

    // Serialize with mdnsStart()/mdnsStop(): mdns_query_ptr() must not run
    // while mdns_free() tears the responder down.
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_started) {
        return false;
    }

    mdns_result_t* results = nullptr;
    esp_err_t error = mdns_query_ptr("_dacp", "_tcp", 1500, 8, &results);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "DACP mDNS query error: %s", esp_err_to_name(error));
    } else if (results == nullptr) {
        // Do not return: a direct SRV query below may still succeed.
        ESP_LOGW(TAG, "DACP mDNS query returned no _dacp._tcp services");
    }

    uint32_t matchedIp = 0;
    uint16_t matchedPort = 0;
    uint32_t senderIpMatch = 0;
    uint16_t senderIpMatchPort = 0;
    int senderIpMatches = 0;
    bool found = false;

    for (mdns_result_t* result = results; result != nullptr;
         result = result->next) {
        ESP_LOGI(TAG, "DACP candidate: instance='%s' port=%u",
                 result->instance_name != nullptr ? result->instance_name
                                                  : "(null)",
                 static_cast<unsigned>(result->port));

        bool matched = result->instance_name != nullptr &&
                       equalsIgnoreCase(result->instance_name, dacpId);
        if (!matched && result->txt != nullptr) {
            for (size_t i = 0; i < result->txt_count; ++i) {
                if (equalsIgnoreCase(result->txt[i].key, "DACP-ID") &&
                    equalsIgnoreCase(result->txt[i].value, dacpId)) {
                    matched = true;
                    break;
                }
            }
        }
        if (!matched && result->instance_name != nullptr) {
            // Some senders append the device name to the DACP-ID.
            matched = std::strstr(result->instance_name, dacpId) != nullptr;
        }

        uint32_t candidateIp = 0;
        for (mdns_ip_addr_t* address = result->addr; address != nullptr;
             address = address->next) {
            if (address->addr.type != ESP_IPADDR_TYPE_V4) {
                continue;
            }
            uint32_t ip = address->addr.u_addr.ip4.addr;
            if (ip != 0) {
                candidateIp = ip;
                break;
            }
        }
        if (candidateIp == 0 || result->port == 0) {
            continue;
        }

        if (senderIp != 0 && candidateIp == senderIp) {
            ++senderIpMatches;
            senderIpMatch = candidateIp;
            senderIpMatchPort = result->port;
        }
        if (matched) {
            if (senderIp != 0 && candidateIp == senderIp) {
                hostIp = candidateIp;
                port = result->port;
                found = true;
                break;
            }
            if (matchedIp == 0) {
                matchedIp = candidateIp;
                matchedPort = result->port;
            }
        }
    }

    if (results != nullptr) {
        mdns_query_results_free(results);
    }

    if (!found && matchedIp != 0) {
        hostIp = matchedIp;
        port = matchedPort;
        found = true;
    }
    if (!found && senderIpMatches == 1) {
        // The instance name did not match, but there is exactly one DACP
        // service at the RTSP peer address.
        hostIp = senderIpMatch;
        port = senderIpMatchPort;
        found = true;
    }

    if (!found) {
        // iOS advertises DACP as "iTunes_Ctrl_<DACP-ID>". A direct SRV
        // query asks for a unicast response and often works when multicast
        // responses are filtered by the access point.
        std::string instance = "iTunes_Ctrl_";
        instance += dacpId;
        found = queryDacpSrvLocked(instance.c_str(), senderIp, hostIp, port);
    }
    if (!found) {
        found = queryDacpSrvLocked(dacpId, senderIp, hostIp, port);
    }

    if (found) {
        ESP_LOGI(TAG, "DACP endpoint for %s: %u.%u.%u.%u:%u", dacpId,
                 static_cast<unsigned>(hostIp & 0xff),
                 static_cast<unsigned>((hostIp >> 8) & 0xff),
                 static_cast<unsigned>((hostIp >> 16) & 0xff),
                 static_cast<unsigned>((hostIp >> 24) & 0xff),
                 static_cast<unsigned>(port));
    } else {
        ESP_LOGW(TAG, "DACP service '%s' not found", dacpId);
    }
    return found;
}

void mdnsLogServices()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_started) {
        return;
    }

    mdns_result_t* results = nullptr;
    esp_err_t error = mdns_query_ptr("_services", "_dns-sd._udp", 1000, 16,
                                      &results);
    if (error == ESP_OK && results != nullptr) {
        for (mdns_result_t* result = results; result != nullptr;
             result = result->next) {
            ESP_LOGI(TAG, "mDNS service type: '%s'",
                     result->instance_name != nullptr ? result->instance_name
                                                      : "(null)");
        }
        mdns_query_results_free(results);
    } else {
        ESP_LOGW(TAG, "mDNS service enumeration returned nothing");
    }

    const struct {
        const char* service;
        const char* proto;
    } types[] = {
        {"_airplay", "_tcp"},
        {"_raop", "_tcp"},
        {"_dacp", "_tcp"},
    };

    for (const auto& type : types) {
        results = nullptr;
        error = mdns_query_ptr(type.service, type.proto, 1000, 8, &results);
        if (error == ESP_OK && results != nullptr) {
            for (mdns_result_t* result = results; result != nullptr;
                 result = result->next) {
                ESP_LOGI(TAG, "mDNS %s.%s: instance='%s' port=%u",
                         type.service, type.proto,
                         result->instance_name != nullptr
                             ? result->instance_name
                             : "(null)",
                         static_cast<unsigned>(result->port));
            }
            mdns_query_results_free(results);
        } else {
            ESP_LOGI(TAG, "mDNS %s.%s: none", type.service, type.proto);
        }
    }
}

} // namespace airplay
