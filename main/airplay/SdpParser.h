#ifndef __AIRPLAY_SDP_PARSER_H__
#define __AIRPLAY_SDP_PARSER_H__

#include <string>

#include "AirPlayTypes.h"

namespace airplay {

bool parseSdp(const std::string& body, SdpInfo& info, std::string& error);

} // namespace airplay

#endif // __AIRPLAY_SDP_PARSER_H__
