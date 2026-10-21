#ifndef __AIRPLAY_TYPES_H__
#define __AIRPLAY_TYPES_H__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace airplay {

constexpr uint16_t kRtspPort = 7000;
constexpr uint32_t kDefaultSampleRate = 44100;
constexpr uint16_t kDefaultChannels = 2;
constexpr uint16_t kDefaultBitsPerSample = 16;
constexpr uint16_t kAlacFrameSamples = 352;
constexpr size_t kMaxSamplesPerChannel = 1024;
constexpr size_t kMaxDecodedSamplesPerChannel = 8192;
constexpr size_t kMaxRtpPacketSize = 16 * 1024;
constexpr size_t kMaxRtspHeaderSize = 16 * 1024;
constexpr size_t kMaxRtspBodySize = 512 * 1024;
constexpr size_t kMaxSdpSize = 16 * 1024;
constexpr size_t kPcmJitterSlots = 384;
constexpr uint32_t kPrebufferMs = 1500;
// Frames older than this are dropped instead of being played as a burst
// after a long scheduling/network gap.
constexpr uint32_t kMaxLateFrameMs = 300;
// Cover-art decoding is CPU heavy (hundreds of ms for a large JPEG) and
// competes with the audio pipeline for PSRAM bandwidth. Require this many
// buffered PCM frames (~28 x 8 ms ≈ 225 ms) before starting a decode so
// the pipeline has enough headroom to ride it out.
constexpr size_t kArtworkMinBufferSlots = 28;
constexpr int kVolumeDefaultDbTimes10 = -150;
constexpr uint32_t kAirPlayLatencySamples = 0;

struct SdpInfo {
    std::string codec = "AppleLossless";
    int payloadType = 96;
    int sampleRate = kDefaultSampleRate;
    int channels = kDefaultChannels;
    int bitsPerSample = kDefaultBitsPerSample;
    int frameLength = kAlacFrameSamples;

    // ALAC fmtp fields: frameLength compatibleVersion bitDepth pb mb kb
    //                  numChannels maxRun maxFrameBytes avgBitRate sampleRate
    int compatibleVersion = 0;
    int pb = 40;
    int mb = 10;
    int kb = 14;
    int maxRun = 255;
    int maxFrameBytes = 0;
    int avgBitRate = 0;

    int minLatency = -1;
    int maxLatency = -1;

    bool encrypted = false;
    std::vector<uint8_t> aesKey;
    std::vector<uint8_t> aesIv;

    bool isValid() const {
        return (codec == "AppleLossless" || codec == "ALAC") &&
               sampleRate > 0 && channels > 0 && bitsPerSample > 0 &&
               frameLength > 0 && frameLength <= 4096;
    }
};

struct AirPlayMetadata {
    std::string title;
    std::string artist;
    std::string album;
    bool hasTitle = false;
    bool hasArtist = false;
    bool hasAlbum = false;
    int durationMs = -1;
};

enum class PlaybackState {
    Stopped,
    Playing,
    Paused,
};

// DACP commands used by the on-device transport buttons to control the
// AirPlay sender (AirPlay 1 has no receiver-side transport control).
enum class RemoteCommand : uint8_t {
    Play,
    Pause,
    PlayPause,
    Next,
    Previous,
};

} // namespace airplay

#endif // __AIRPLAY_TYPES_H__
