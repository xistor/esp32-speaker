#ifndef __AIRPLAY_MDNS_H__
#define __AIRPLAY_MDNS_H__

#include <cstdint>

#include "esp_err.h"

namespace airplay {

esp_err_t mdnsStart(const char* deviceName, const char* mac, bool artworkCapable);
void mdnsStop();
void mdnsSetSessionActive(bool active);

// Resolves the sender's DACP control endpoint. The DACP-ID is taken from the
// ANNOUNCE header; senderIp is the RTSP peer address. Returns the TCP host
// address and port to send /ctrl-int/1/... requests to.
bool mdnsFindDacp(const char* dacpId, uint32_t senderIp,
                  uint32_t& hostIp, uint16_t& port);

// One-shot diagnostic dump of the mDNS services visible on the network.
// Useful when the sender does not advertise _dacp._tcp.
void mdnsLogServices();

} // namespace airplay

#endif // __AIRPLAY_MDNS_H__
