#include "DacpClient.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "AirPlayMdns.h"
#include "esp_log.h"
#include "esp_pthread.h"

namespace airplay {

namespace {

constexpr const char* TAG = "AirPlayDacp";
constexpr int kConnectTimeoutSec = 3;
constexpr int kIoTimeoutSec = 3;

const char* commandPath(RemoteCommand command)
{
    switch (command) {
    case RemoteCommand::Play:
        return "play";
    case RemoteCommand::Pause:
        return "pause";
    case RemoteCommand::PlayPause:
        return "playpause";
    case RemoteCommand::Next:
        return "nextitem";
    case RemoteCommand::Previous:
        return "previtem";
    }
    return nullptr;
}

bool sendAll(int fd, const void* data, size_t length)
{
    const uint8_t* pointer = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < length) {
        int result = send(fd, pointer + sent, length - sent, 0);
        if (result > 0) {
            sent += static_cast<size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool connectWithTimeout(int fd, const struct sockaddr_in& address)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }

    int result = connect(fd, reinterpret_cast<const struct sockaddr*>(&address),
                         sizeof(address));
    if (result != 0 && errno != EINPROGRESS) {
        return false;
    }
    if (result != 0) {
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(fd, &writeSet);
        struct timeval timeout = {};
        timeout.tv_sec = kConnectTimeoutSec;
        int ready = select(fd + 1, nullptr, &writeSet, nullptr, &timeout);
        if (ready <= 0) {
            return false;
        }
        int socketError = 0;
        socklen_t errorLength = sizeof(socketError);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError,
                       &errorLength) != 0 ||
            socketError != 0) {
            return false;
        }
    }

    return fcntl(fd, F_SETFL, flags) == 0;
}

} // namespace

DacpClient::~DacpClient()
{
    stop();
}

bool DacpClient::start()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_running) {
        return true;
    }

    _stop = false;
    _running = true;

    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.thread_name = "airplay_dacp";
    cfg.prio = 4;
    cfg.stack_size = 8192;
    cfg.pin_to_core = 0;
    cfg.inherit_cfg = false;
    esp_pthread_set_cfg(&cfg);

    try {
        _thread = std::thread(&DacpClient::workerLoop, this);
    } catch (...) {
        _running = false;
        return false;
    }

    ESP_LOGI(TAG, "DACP client started");
    return true;
}

void DacpClient::stop()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_running) {
            return;
        }
        _stop = true;
        _queue.clear();
    }
    _cv.notify_all();

    if (_thread.joinable()) {
        _thread.join();
    }

    std::lock_guard<std::mutex> lock(_mutex);
    _running = false;
    _stop = false;
    ESP_LOGI(TAG, "DACP client stopped");
}

void DacpClient::setSession(const std::string& dacpId,
                            const std::string& activeRemote, uint32_t senderIp)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_dacpId == dacpId && _activeRemote == activeRemote &&
        _senderIp == senderIp) {
        return;
    }
    _dacpId = dacpId;
    _activeRemote = activeRemote;
    _senderIp = senderIp;
    _endpointIp = 0;
    _endpointPort = 0;
    _diagnosticsLogged = false;
    ++_generation;

    // Resolve the sender's DACP endpoint while playback is starting so the
    // first button press does not have to pay for the mDNS query.
    if (!_dacpId.empty() && !_activeRemote.empty() && _senderIp != 0) {
        ESP_LOGI(TAG, "DACP session: id='%s' active-remote='%s'",
                 _dacpId.c_str(), _activeRemote.c_str());
        // Discovery is started from RECORD (or from the first button press)
        // rather than ANNOUNCE, so no mDNS work runs during the initial
        // audio setup.
    } else {
        ESP_LOGW(TAG, "DACP session without id/active-remote; buttons disabled");
    }
}

void DacpClient::clearSession()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _dacpId.clear();
    _activeRemote.clear();
    _senderIp = 0;
    _endpointIp = 0;
    _endpointPort = 0;
    _queue.clear();
    _discoverRequested = false;
    _diagnosticsLogged = false;
    ++_generation;
}

void DacpClient::requestDiscovery()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_dacpId.empty() && !_activeRemote.empty() && _senderIp != 0) {
        _discoverRequested = true;
        _cv.notify_one();
    }
}

bool DacpClient::send(RemoteCommand command)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_running || _dacpId.empty() || _activeRemote.empty() ||
        _senderIp == 0) {
        return false;
    }
    if (_queue.size() >= 4) {
        _queue.pop_front();
    }
    _queue.push_back(command);
    _cv.notify_one();
    return true;
}

void DacpClient::workerLoop()
{
    while (true) {
        RemoteCommand command = RemoteCommand::PlayPause;
        std::string dacpId;
        std::string activeRemote;
        uint32_t senderIp = 0;
        uint32_t generation = 0;
        bool discover = false;

        {
            std::unique_lock<std::mutex> lock(_mutex);
            _cv.wait(lock, [this] {
                return _stop || _discoverRequested || !_queue.empty();
            });
            if (_stop) {
                break;
            }

            // Eager discovery runs before queued commands so the first
            // button press usually finds a cached endpoint.
            if (_discoverRequested) {
                _discoverRequested = false;
                discover = true;
            } else {
                command = _queue.front();
                _queue.pop_front();
            }
            dacpId = _dacpId;
            activeRemote = _activeRemote;
            senderIp = _senderIp;
            generation = _generation;
        }

        if (dacpId.empty() || activeRemote.empty() || senderIp == 0) {
            if (!discover) {
                ESP_LOGW(TAG, "No active AirPlay DACP session");
            }
            continue;
        }

        if (discover) {
            discoverEndpoint(dacpId, senderIp, generation);
            continue;
        }

        uint32_t hostIp = 0;
        uint16_t port = 0;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (generation == _generation) {
                hostIp = _endpointIp;
                port = _endpointPort;
            }
        }

        if (hostIp == 0 || port == 0) {
            discoverEndpoint(dacpId, senderIp, generation);

            std::lock_guard<std::mutex> lock(_mutex);
            if (generation != _generation) {
                continue;
            }
            hostIp = _endpointIp;
            port = _endpointPort;
        }

        if (hostIp == 0 || port == 0) {
            if (!_diagnosticsLogged) {
                _diagnosticsLogged = true;
                mdnsLogServices();
            }
            ESP_LOGW(TAG, "DACP endpoint unavailable; command dropped");
            continue;
        }

        // The session may have been torn down while resolving or waiting.
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_stop || generation != _generation) {
                continue;
            }
        }

        if (!sendCommand(hostIp, port, activeRemote, command)) {
            // The endpoint may have moved (e.g. the sender restarted).
            // Drop the cache so the next press resolves it again.
            std::lock_guard<std::mutex> lock(_mutex);
            if (_endpointIp == hostIp && _endpointPort == port) {
                _endpointIp = 0;
                _endpointPort = 0;
            }
        }
    }
    ESP_LOGI(TAG, "DACP worker stopped");
}

void DacpClient::discoverEndpoint(const std::string& dacpId,
                                  uint32_t senderIp, uint32_t generation)
{
    uint32_t hostIp = 0;
    uint16_t port = 0;
    if (!mdnsFindDacp(dacpId.c_str(), senderIp, hostIp, port)) {
        return;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (_stop || generation != _generation) {
        return;
    }
    _endpointIp = hostIp;
    _endpointPort = port;
}

bool DacpClient::sendCommand(uint32_t hostIp, uint16_t port,
                             const std::string& activeRemote,
                             RemoteCommand command)
{
    const char* path = commandPath(command);
    if (path == nullptr) {
        return false;
    }

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGW(TAG, "DACP socket failed: errno=%d", errno);
        return false;
    }

    struct timeval timeout = {};
    timeout.tv_sec = kIoTimeoutSec;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = hostIp;

    if (!connectWithTimeout(fd, address)) {
        ESP_LOGW(TAG, "DACP connect failed: errno=%d", errno);
        close(fd);
        return false;
    }

    char host[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &address.sin_addr, host, sizeof(host)) == nullptr) {
        snprintf(host, sizeof(host), "0.0.0.0");
    }

    char request[512];
    int length = snprintf(request, sizeof(request),
                          "GET /ctrl-int/1/%s HTTP/1.1\r\n"
                          "Host: %s:%u\r\n"
                          "Active-Remote: %s\r\n"
                          "Content-Length: 0\r\n"
                          "Connection: close\r\n"
                          "\r\n",
                          path, host, static_cast<unsigned>(port),
                          activeRemote.c_str());
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(request)) {
        close(fd);
        return false;
    }

    if (!sendAll(fd, request, static_cast<size_t>(length))) {
        ESP_LOGW(TAG, "DACP %s send failed: errno=%d", path, errno);
        close(fd);
        return false;
    }

    char response[256] = {};
    int received = 0;
    do {
        received = recv(fd, response, sizeof(response) - 1, 0);
    } while (received < 0 && errno == EINTR);
    close(fd);

    if (received <= 0) {
        ESP_LOGW(TAG, "DACP %s: no response", path);
        return false;
    }
    response[received] = '\0';

    bool ok = strstr(response, " 200 ") != nullptr ||
              strstr(response, " 204 ") != nullptr;
    if (ok) {
        ESP_LOGI(TAG, "DACP %s OK", path);
    } else {
        char* lineEnd = strstr(response, "\r\n");
        if (lineEnd != nullptr) {
            *lineEnd = '\0';
        }
        ESP_LOGW(TAG, "DACP %s failed: %s", path, response);
    }
    return ok;
}

} // namespace airplay
