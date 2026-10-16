#ifndef __WEB_SERVER_H__
#define __WEB_SERVER_H__

#include "esp_err.h"

class WebServer {
public:
    WebServer() = default;
    ~WebServer();
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // Brings up the HTTP server. httpd binds to every interface, so the UI is
    // reachable over the SoftAP while the device is unconfigured and over the
    // LAN address once it joins the user's network. Idempotent.
    esp_err_t start();

    // Tear the HTTP server down. Idempotent.
    esp_err_t stop();

    bool isRunning() const { return _running; }

private:
    static constexpr const char* _TAG = "WebServer";

    esp_err_t httpStart();
    esp_err_t httpStop();

    bool _running = false;
    void* _server = nullptr;  // httpd_handle_t kept as void* to avoid leaking the
                              // esp_http_server type into this header.
};

#endif // __WEB_SERVER_H__