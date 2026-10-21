#ifndef __AIRPLAY_ALAC_DECODER_H__
#define __AIRPLAY_ALAC_DECODER_H__

#include <cstddef>
#include <cstdint>

#include "AirPlayTypes.h"

namespace airplay {

class AlacDecoder {
public:
    AlacDecoder() = default;
    ~AlacDecoder();

    AlacDecoder(const AlacDecoder&) = delete;
    AlacDecoder& operator=(const AlacDecoder&) = delete;

    bool open(const SdpInfo& sdp);
    void close();
    void reset();

    bool isOpen() const { return _handle != nullptr; }

    // Decodes one ALAC frame. Returns false on malformed input; the caller
    // can keep going with the next RTP packet.
    bool decode(const uint8_t* data, size_t len, int16_t* pcm,
                size_t pcmCapacityBytes, size_t& decodedBytes,
                size_t& consumedBytes);

private:
    void buildMagicCookie(const SdpInfo& sdp, uint8_t cookie[24]);

    void* _handle = nullptr;
    uint8_t _magicCookie[24] = {};
    uint32_t _frameSamples = 0;
    uint16_t _channels = 0;
};

} // namespace airplay

#endif // __AIRPLAY_ALAC_DECODER_H__
