#include "SdpParser.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

#include "AirPlayCrypto.h"
#include "esp_log.h"

namespace airplay {

namespace {

constexpr const char* TAG = "AirPlaySdp";

std::string trim(const std::string& text)
{
    size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool startsWith(const std::string& text, const char* prefix)
{
    return text.rfind(prefix, 0) == 0;
}

int parseInt(const std::string& text)
{
    return std::atoi(text.c_str());
}

} // namespace

bool parseSdp(const std::string& body, SdpInfo& info, std::string& error)
{
    info = SdpInfo();
    error.clear();

    std::istringstream stream(body);
    std::string line;
    std::string fmtp;
    std::string rtpmap;
    std::string rsaAesKeyB64;
    std::string aesIvB64;

    bool sawAudio = false;
    int payloadType = 96;

    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        if (startsWith(line, "m=audio ")) {
            sawAudio = true;
            const char* marker = strstr(line.c_str(), "RTP/AVP ");
            if (marker != nullptr) {
                payloadType = std::atoi(marker + strlen("RTP/AVP "));
            }
            continue;
        }
        if (startsWith(line, "a=rtpmap:")) {
            rtpmap = line.substr(strlen("a=rtpmap:"));
            continue;
        }
        if (startsWith(line, "a=fmtp:")) {
            fmtp = line.substr(strlen("a=fmtp:"));
            continue;
        }
        if (startsWith(line, "a=rsaaeskey:")) {
            rsaAesKeyB64 = line.substr(strlen("a=rsaaeskey:"));
            continue;
        }
        if (startsWith(line, "a=aesiv:")) {
            aesIvB64 = line.substr(strlen("a=aesiv:"));
            continue;
        }
        if (startsWith(line, "a=min-latency:")) {
            info.minLatency = parseInt(line.substr(strlen("a=min-latency:")));
            continue;
        }
        if (startsWith(line, "a=max-latency:")) {
            info.maxLatency = parseInt(line.substr(strlen("a=max-latency:")));
            continue;
        }
    }

    if (!sawAudio) {
        error = "SDP has no audio media section";
        return false;
    }

    info.payloadType = payloadType;

    if (!rtpmap.empty()) {
        // "<pt> <codec>/<rate>/<channels>"
        char codec[64] = {};
        int rate = 0;
        int channels = 0;
        int pt = 0;
        int matched = std::sscanf(rtpmap.c_str(), "%d %63[^/]/%d/%d", &pt,
                                  codec, &rate, &channels);
        if (matched >= 1) {
            info.payloadType = pt;
        }
        if (matched >= 2) {
            info.codec = codec;
        }
        if (matched >= 3 && rate > 0) {
            info.sampleRate = rate;
        }
        if (matched >= 4 && channels > 0) {
            info.channels = channels;
        }
    }

    if (!fmtp.empty()) {
        // "<pt> <frameLength> <compatibleVersion> <bitDepth> <pb> <mb> <kb>
        //  <numChannels> <maxRun> <maxFrameBytes> <avgBitRate> <sampleRate>"
        int pt = 0;
        int frameLength = 0;
        int compatibleVersion = 0;
        int bitDepth = 0;
        int pb = 0;
        int mb = 0;
        int kb = 0;
        int numChannels = 0;
        int maxRun = 0;
        int maxFrameBytes = 0;
        int avgBitRate = 0;
        int sampleRate = 0;

        int matched = std::sscanf(
            fmtp.c_str(),
            "%d %d %d %d %d %d %d %d %d %d %d %d", &pt, &frameLength,
            &compatibleVersion, &bitDepth, &pb, &mb, &kb, &numChannels,
            &maxRun, &maxFrameBytes, &avgBitRate, &sampleRate);

        if (matched >= 1) {
            info.payloadType = pt;
        }
        if (matched >= 2 && frameLength > 0) {
            info.frameLength = frameLength;
        }
        if (matched >= 3) {
            info.compatibleVersion = compatibleVersion;
        }
        if (matched >= 4 && bitDepth > 0) {
            info.bitsPerSample = bitDepth;
        }
        if (matched >= 5) {
            info.pb = pb;
        }
        if (matched >= 6) {
            info.mb = mb;
        }
        if (matched >= 7) {
            info.kb = kb;
        }
        if (matched >= 8 && numChannels > 0) {
            info.channels = numChannels;
        }
        if (matched >= 9) {
            info.maxRun = maxRun;
        }
        if (matched >= 10) {
            info.maxFrameBytes = maxFrameBytes;
        }
        if (matched >= 11) {
            info.avgBitRate = avgBitRate;
        }
        if (matched >= 12 && sampleRate > 0) {
            info.sampleRate = sampleRate;
        }
    }

    if (info.codec.empty()) {
        info.codec = "AppleLossless";
    }

    if (info.codec != "AppleLossless" && info.codec != "ALAC") {
        error = "Unsupported SDP codec: " + info.codec;
        return false;
    }

    if (!rsaAesKeyB64.empty() || !aesIvB64.empty()) {
        if (rsaAesKeyB64.empty() || aesIvB64.empty()) {
            error = "SDP has only one of rsaaeskey/aesiv";
            return false;
        }

        std::vector<uint8_t> encryptedKey;
        std::vector<uint8_t> iv;
        if (!AirPlayCrypto::base64Decode(rsaAesKeyB64, encryptedKey)) {
            error = "Invalid base64 in rsaaeskey";
            return false;
        }
        if (!AirPlayCrypto::base64Decode(aesIvB64, iv) || iv.size() < 16) {
            error = "Invalid base64 in aesiv";
            return false;
        }

        if (!AirPlayCrypto::instance().decryptAesKey(
                encryptedKey.data(), encryptedKey.size(), info.aesKey)) {
            error = "RSA decrypt of rsaaeskey failed";
            return false;
        }

        info.aesIv.assign(iv.begin(), iv.begin() + 16);
        info.encrypted = true;

        ESP_LOGI(TAG, "SDP encryption: AES key=%u bytes, iv=%u bytes",
                 static_cast<unsigned>(info.aesKey.size()),
                 static_cast<unsigned>(info.aesIv.size()));
    }

    if (!info.isValid()) {
        error = "Invalid ALAC parameters in SDP";
        return false;
    }

    ESP_LOGI(TAG, "SDP: codec=%s pt=%d %d Hz %d ch %d bit frame=%d",
             info.codec.c_str(), info.payloadType, info.sampleRate,
             info.channels, info.bitsPerSample, info.frameLength);
    return true;
}

} // namespace airplay
