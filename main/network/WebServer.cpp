#include "WebServer.h"
#include "Setting.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "WifiMgr.h"
#include <cstdio>
#include <cstring>

constexpr const char* WebServer::_TAG;

// Map a file extension to a Content-Type. Returns text/plain for unknown types.
static const char* content_type_for(const char* uri) {
    const char* dot = strrchr(uri, '.');
    if (!dot) return "text/plain";
    if (strcmp(dot, ".html") == 0) return "text/html";
    if (strcmp(dot, ".css")  == 0) return "text/css";
    if (strcmp(dot, ".js")   == 0) return "application/javascript";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".png")  == 0) return "image/png";
    if (strcmp(dot, ".jpg")  == 0) return "image/jpeg";
    if (strcmp(dot, ".ico")  == 0) return "image/x-icon";
    if (strcmp(dot, ".svg")  == 0) return "image/svg+xml";
    return "text/plain";
}

// Stream a file from SPIFFS into the HTTP response. Sends 404 if missing.
static esp_err_t serve_file(httpd_req_t *req, const char* spiffs_path) {
    FILE* f = fopen(spiffs_path, "r");
    if (!f) {
        ESP_LOGW("WebServer", "Not found: %s", spiffs_path);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, content_type_for(spiffs_path));

    char chunk[1024];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, nullptr, 0);  // terminate chunked response
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req) {
    return serve_file(req, "/spiffs/www/index.html");
}

// Catch-all static file handler — strips the leading "/" and prepends /spiffs/.
// Uses memcpy rather than snprintf so the compiler's format-truncation
// analyser doesn't object to "/spiffs" + the full URI.
static esp_err_t static_file_handler(httpd_req_t *req) {
    constexpr size_t prefix_len = sizeof("/spiffs") - 1;  // 7
    char path[sizeof(req->uri) + prefix_len + 1];        // 513 + 7 + 1 = 521
    memcpy(path, "/spiffs", prefix_len);
    memcpy(path + prefix_len, req->uri, sizeof(req->uri));  // copies NUL too
    return serve_file(req, path);
}

// Tiny JSON status endpoint — reports whichever address the UI is actually
// reachable on: the LAN address once the device has joined the user's network,
// otherwise the SoftAP address used for setup.
static esp_err_t status_handler(httpd_req_t *req) {
    auto& wifi = WifiMgr::instance();
    esp_netif_ip_info_t ip;
    char body[80];

    if (wifi.isStaConnected() && esp_netif_get_ip_info(wifi.getStaNetif(), &ip) == ESP_OK) {
        snprintf(body, sizeof(body), "{\"ip\":\"" IPSTR "\",\"mode\":\"sta\"}", IP2STR(&ip.ip));
    } else if (wifi.getApNetif() && esp_netif_get_ip_info(wifi.getApNetif(), &ip) == ESP_OK) {
        snprintf(body, sizeof(body), "{\"ip\":\"" IPSTR "\",\"mode\":\"ap\"}", IP2STR(&ip.ip));
    } else {
        snprintf(body, sizeof(body), "{\"ip\":\"unknown\"}");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, strlen(body));
    return ESP_OK;
}

// Stub command endpoint — wired into SpeakerApp playControlCb later.
static esp_err_t cmd_handler(httpd_req_t *req) {
    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char cmd[16] = {0};
        if (httpd_query_key_value(query, "c", cmd, sizeof(cmd)) == ESP_OK) {
            ESP_LOGI("WebServer", "cmd: %s", cmd);
            // TODO: forward to SpeakerApp::playControlCb when integrating.
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", 10);
    return ESP_OK;
}

// GET /api/wifi — return saved SSID + current STA status. Password is never echoed.
static esp_err_t wifi_get_handler(httpd_req_t *req) {
    auto& wifi = WifiMgr::instance();
    char ssid[33] = {0};
    char pass[65] = {0};
    bool configured = Setting::instance().getWifiCredentials(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK;

    char body[256];
    if (configured && wifi.isStaConnected()) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(wifi.getStaNetif(), &ip) == ESP_OK) {
            snprintf(body, sizeof(body),
                     "{\"configured\":true,\"ssid\":\"%s\",\"connected\":true,\"ip\":\"" IPSTR "\"}",
                     ssid, IP2STR(&ip.ip));
        } else {
            snprintf(body, sizeof(body),
                     "{\"configured\":true,\"ssid\":\"%s\",\"connected\":true}", ssid);
        }
    } else if (configured) {
        snprintf(body, sizeof(body),
                 "{\"configured\":true,\"ssid\":\"%s\",\"connected\":false}", ssid);
    } else {
        snprintf(body, sizeof(body),
                 "{\"configured\":false,\"connected\":false}");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, strlen(body));
    return ESP_OK;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode %XX escapes in place (output is never longer than the input).
// The web UI encodes values with encodeURIComponent(), which turns reserved
// characters into "%XX" but leaves '+' as a literal plus, so '+' is kept as-is.
static void url_decode(char* s) {
    char* out = s;
    for (char* in = s; *in != '\0'; ++in) {
        int hi = -1, lo = -1;
        if (*in == '%' && (hi = hex_value(in[1])) >= 0 && (lo = hex_value(in[2])) >= 0) {
            *out++ = (char)((hi << 4) | lo);
            in += 2;
        } else {
            *out++ = *in;
        }
    }
    *out = '\0';
}

// POST /api/wifi — body is application/x-www-form-urlencoded: ssid=...&pass=...
// Body values are percent-encoded, so they must be decoded before use; storing
// the raw "%XX" text makes the STA handshake fail with reason 15.
// Saves creds to NVS and triggers an STA connect.
static esp_err_t wifi_post_handler(httpd_req_t *req) {
    constexpr size_t ssid_max = 32;  // Wi-Fi SSID limit
    constexpr size_t pass_max = 64;  // WPA2 passphrase limit

    // Worst case body: every credential character escaped as "%XX".
    char buf[3 * (ssid_max + pass_max) + 16] = {0};
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(buf)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"empty or oversized body\"}", -1);
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total) {
        int n = httpd_req_recv(req, buf + received, total - received);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_send(req, "{\"ok\":false,\"error\":\"recv failed\"}", -1);
            return ESP_FAIL;
        }
        received += n;
    }
    buf[received] = '\0';

    char ssid[3 * ssid_max + 1] = {0};
    char pass[3 * pass_max + 1] = {0};
    if (httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"missing ssid\"}", -1);
        return ESP_FAIL;
    }
    url_decode(ssid);
    if (ssid[0] == '\0' || strlen(ssid) > ssid_max) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid ssid\"}", -1);
        return ESP_FAIL;
    }
    // pass is optional (for open networks).
    if (httpd_query_key_value(buf, "pass", pass, sizeof(pass)) == ESP_OK) {
        url_decode(pass);
    }
    if (strlen(pass) > pass_max) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid password\"}", -1);
        return ESP_FAIL;
    }

    ESP_LOGI("WebServer", "WiFi config: ssid='%s' pass=<%u chars>", ssid, (unsigned)strlen(pass));
    esp_err_t err = WifiMgr::instance().connectSta(ssid, pass);
    if (err != ESP_OK) {
        char err_body[96];
        snprintf(err_body, sizeof(err_body), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, err_body, strlen(err_body));
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", 10);
    return ESP_OK;
}

WebServer::~WebServer() {
    // Tear down just the HTTP server. Don't touch event-handler state here:
    // by the time the destructor runs the owner should already have called
    // stop(), and either the event loop has been destroyed (so the handler
    // registrations are already gone) or it's about to be.
    httpStop();
}

esp_err_t WebServer::httpStart() {
    if (_running) return ESP_OK;

    // 1. Mount SPIFFS.
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path      = "/spiffs",
        .partition_label = "storage",
        .max_files      = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&spiffs_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(_TAG, "SPIFFS mount failed: %s (did you flash the image?)",
                 esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(_TAG, "SPIFFS: %u / %u bytes used", (unsigned)used, (unsigned)total);

    // 2. Start HTTP server.
    httpd_config_t http_conf = HTTPD_DEFAULT_CONFIG();
    http_conf.max_uri_handlers = 16;
    http_conf.stack_size = 4096;
    ret = httpd_start(reinterpret_cast<httpd_handle_t*>(&_server), &http_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(_TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        esp_vfs_spiffs_unregister("storage");
        return ret;
    }

    // 3. Register URI handlers.
    // httpd_uri_t has 4 base fields plus a conditional is_websocket bool, so we
    // brace-init each entry and then memset the rest to 0 to satisfy
    // -Wmissing-field-initializers regardless of whether is_websocket exists.
    httpd_uri_t uris[7];
    memset(uris, 0, sizeof(uris));
    uris[0] = { .uri = "/",         .method = HTTP_GET,  .handler = index_handler };
    uris[1] = { .uri = "/index.html", .method = HTTP_GET, .handler = index_handler };
    uris[2] = { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler };
    uris[3] = { .uri = "/api/cmd",  .method = HTTP_GET,  .handler = cmd_handler };
    uris[4] = { .uri = "/api/wifi", .method = HTTP_GET,  .handler = wifi_get_handler };
    uris[5] = { .uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post_handler };
    // catch-all static — must be last
    uris[6] = { .uri = "/*",        .method = HTTP_GET,  .handler = static_file_handler };
    for (auto& u : uris) {
        httpd_register_uri_handler(reinterpret_cast<httpd_handle_t>(_server), &u);
    }

    _running = true;
    ESP_LOGI(_TAG, "Web server started on port %d", http_conf.server_port);
    return ESP_OK;
}

esp_err_t WebServer::httpStop() {
    if (!_running) return ESP_OK;
    if (_server) {
        httpd_stop(reinterpret_cast<httpd_handle_t>(_server));
        _server = nullptr;
    }
    esp_vfs_spiffs_unregister("storage");
    _running = false;
    ESP_LOGI(_TAG, "Web server stopped");
    return ESP_OK;
}

esp_err_t WebServer::start() {
    if (_running) return ESP_OK;
    return httpStart();
}

esp_err_t WebServer::stop() {
    return httpStop();
}
