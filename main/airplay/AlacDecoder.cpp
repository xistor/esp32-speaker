#include "AlacDecoder.h"

#include <cstring>

#include "esp_audio_dec.h"
#include "decoder/impl/esp_alac_dec.h"
#include "esp_log.h"

namespace airplay {

namespace {

constexpr const char* TAG = "AirPlayAlac";

void writeBe16(uint8_t* out, uint16_t value)
{
    out[0] = static_cast<uint8_t>(value >> 8);
    out[1] = static_cast<uint8_t>(value);
}

void writeBe32(uint8_t* out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value >> 24);
    out[1] = static_cast<uint8_t>(value >> 16);
    out[2] = static_cast<uint8_t>(value >> 8);
    out[3] = static_cast<uint8_t>(value);
}

} // namespace

AlacDecoder::~AlacDecoder()
{
    close();
}

bool AlacDecoder::open(const SdpInfo& sdp)
{
    close();

    buildMagicCookie(sdp, _magicCookie);

    esp_alac_dec_cfg_t cfg = {};
    cfg.codec_spec_info = _magicCookie;
    cfg.spec_info_len = sizeof(_magicCookie);

    esp_audio_err_t err =
        esp_alac_dec_open(&cfg, sizeof(cfg), &_handle);
    if (err != ESP_AUDIO_ERR_OK) {
        _handle = nullptr;
        ESP_LOGE(TAG, "esp_alac_dec_open failed: %d", static_cast<int>(err));
        return false;
    }

    _frameSamples = static_cast<uint32_t>(sdp.frameLength);
    _channels = static_cast<uint16_t>(sdp.channels);

    ESP_LOGI(TAG, "ALAC open: %u Hz, %u ch, %u bits, %u samples/frame",
             static_cast<unsigned>(sdp.sampleRate),
             static_cast<unsigned>(sdp.channels),
             static_cast<unsigned>(sdp.bitsPerSample),
             static_cast<unsigned>(sdp.frameLength));
    return true;
}

void AlacDecoder::close()
{
    if (_handle != nullptr) {
        esp_alac_dec_close(_handle);
        _handle = nullptr;
    }
    _frameSamples = 0;
    _channels = 0;
}

void AlacDecoder::reset()
{
    if (_handle != nullptr) {
        esp_alac_dec_reset(_handle);
    }
}

bool AlacDecoder::decode(const uint8_t* data, size_t len, int16_t* pcm,
                         size_t pcmCapacityBytes, size_t& decodedBytes,
                         size_t& consumedBytes)
{
    decodedBytes = 0;
    consumedBytes = 0;
    if (_handle == nullptr || data == nullptr || len == 0 || pcm == nullptr) {
        return false;
    }

    esp_audio_dec_in_raw_t raw = {};
    raw.buffer = const_cast<uint8_t*>(data);
    raw.len = static_cast<uint32_t>(len);

    esp_audio_dec_out_frame_t frame = {};
    frame.buffer = reinterpret_cast<uint8_t*>(pcm);
    frame.len = static_cast<uint32_t>(pcmCapacityBytes);

    esp_audio_dec_info_t info = {};
    esp_audio_err_t err = esp_alac_dec_decode(_handle, &raw, &frame, &info);
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "ALAC decode failed: %d (in=%u, out=%u)",
                 static_cast<int>(err), static_cast<unsigned>(len),
                 static_cast<unsigned>(pcmCapacityBytes));
        return false;
    }

    decodedBytes = frame.decoded_size;
    consumedBytes = raw.consumed;
    return decodedBytes > 0;
}

void AlacDecoder::buildMagicCookie(const SdpInfo& sdp, uint8_t cookie[24])
{
    std::memset(cookie, 0, 24);
    writeBe32(cookie + 0, static_cast<uint32_t>(sdp.frameLength));
    cookie[4] = static_cast<uint8_t>(sdp.compatibleVersion);
    cookie[5] = static_cast<uint8_t>(sdp.bitsPerSample);
    cookie[6] = static_cast<uint8_t>(sdp.pb);
    cookie[7] = static_cast<uint8_t>(sdp.mb);
    cookie[8] = static_cast<uint8_t>(sdp.kb);
    cookie[9] = static_cast<uint8_t>(sdp.channels);
    writeBe16(cookie + 10, static_cast<uint16_t>(sdp.maxRun));
    writeBe32(cookie + 12, static_cast<uint32_t>(sdp.maxFrameBytes));
    writeBe32(cookie + 16, static_cast<uint32_t>(sdp.avgBitRate));
    writeBe32(cookie + 20, static_cast<uint32_t>(sdp.sampleRate));
}

} // namespace airplay
