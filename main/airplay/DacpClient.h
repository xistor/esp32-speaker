#ifndef __AIRPLAY_DACP_CLIENT_H__
#define __AIRPLAY_DACP_CLIENT_H__

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "AirPlayTypes.h"

namespace airplay {

// Sends AirPlay 1 remote-control commands to the sender's DACP server.
// The sender advertises _dacp._tcp and expects requests on
// /ctrl-int/1/<command> with the Active-Remote token from ANNOUNCE.
class DacpClient {
public:
    DacpClient() = default;
    ~DacpClient();

    DacpClient(const DacpClient&) = delete;
    DacpClient& operator=(const DacpClient&) = delete;

    bool start();
    void stop();

    void setSession(const std::string& dacpId,
                    const std::string& activeRemote, uint32_t senderIp);
    void clearSession();
    void requestDiscovery();

    bool send(RemoteCommand command);

private:
    void workerLoop();
    void discoverEndpoint(const std::string& dacpId, uint32_t senderIp,
                          uint32_t generation);
    bool sendCommand(uint32_t hostIp, uint16_t port,
                     const std::string& activeRemote,
                     RemoteCommand command);

    std::thread _thread;
    std::mutex _mutex;
    std::condition_variable _cv;
    std::deque<RemoteCommand> _queue;
    bool _running = false;
    bool _stop = false;
    bool _discoverRequested = false;
    bool _diagnosticsLogged = false;

    std::string _dacpId;
    std::string _activeRemote;
    uint32_t _senderIp = 0;
    uint32_t _generation = 0;

    // Cached mDNS resolution for the current session. Re-resolved on failure.
    uint32_t _endpointIp = 0;
    uint16_t _endpointPort = 0;
};

} // namespace airplay

#endif // __AIRPLAY_DACP_CLIENT_H__
