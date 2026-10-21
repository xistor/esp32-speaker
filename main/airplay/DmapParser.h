#ifndef __AIRPLAY_DMAP_PARSER_H__
#define __AIRPLAY_DMAP_PARSER_H__

#include <cstddef>
#include <cstdint>
#include <string>

namespace airplay {

struct DmapMetadata {
    std::string title;
    std::string artist;
    std::string album;
    int durationMs = -1;
};

bool parseDmap(const uint8_t* data, size_t len, DmapMetadata& metadata);

} // namespace airplay

#endif // __AIRPLAY_DMAP_PARSER_H__
