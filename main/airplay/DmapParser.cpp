#include "DmapParser.h"

#include <cstring>

namespace airplay {

namespace {

uint32_t readBe32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint32_t tagValue(const char* tag)
{
    return (static_cast<uint32_t>(static_cast<uint8_t>(tag[0])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(tag[1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(tag[2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(tag[3]));
}

void parseItems(const uint8_t* data, size_t len, DmapMetadata& metadata,
                int depth)
{
    if (depth > 8) {
        return;
    }

    size_t offset = 0;
    while (offset + 8 <= len) {
        uint32_t tag = readBe32(data + offset);
        uint32_t itemLen = readBe32(data + offset + 4);
        offset += 8;
        if (itemLen > len - offset) {
            return;
        }

        const uint8_t* value = data + offset;
        offset += itemLen;

        if (tag == tagValue("mlit")) {
            parseItems(value, itemLen, metadata, depth + 1);
        } else if (tag == tagValue("minm")) {
            metadata.title.assign(reinterpret_cast<const char*>(value),
                                  itemLen);
        } else if (tag == tagValue("asar")) {
            metadata.artist.assign(reinterpret_cast<const char*>(value),
                                   itemLen);
        } else if (tag == tagValue("asal")) {
            metadata.album.assign(reinterpret_cast<const char*>(value),
                                  itemLen);
        } else if (tag == tagValue("astm")) {
            if (itemLen >= 4) {
                metadata.durationMs = static_cast<int>(readBe32(value));
            }
        }
    }
}

} // namespace

bool parseDmap(const uint8_t* data, size_t len, DmapMetadata& metadata)
{
    metadata = DmapMetadata();
    if (data == nullptr || len < 8) {
        return false;
    }

    parseItems(data, len, metadata, 0);
    return !metadata.title.empty() || !metadata.artist.empty() ||
           !metadata.album.empty() || metadata.durationMs >= 0;
}

} // namespace airplay
