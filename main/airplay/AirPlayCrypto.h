#ifndef __AIRPLAY_CRYPTO_H__
#define __AIRPLAY_CRYPTO_H__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"
#include "psa/crypto.h"

namespace airplay {

class AirPlayCrypto {
public:
    static AirPlayCrypto& instance();

    esp_err_t init();
    bool isReady() const { return _ready; }

    // Apple-Challenge response: RSA PKCS#1 v1.5 signature over
    // challenge || local IPv4 (network byte order) || MAC.
    std::string appleResponse(const uint8_t* challenge, size_t challengeLen,
                              uint32_t ipv4, const uint8_t mac[6]);

    // RSA-OAEP(SHA-1) decrypt of the SDP rsaaeskey value.
    bool decryptAesKey(const uint8_t* encrypted, size_t encryptedLen,
                       std::vector<uint8_t>& aesKey);

    // AES-128-CBC, no padding. The caller supplies the per-packet IV.
    psa_key_id_t importAesKey(const uint8_t* key, size_t keyLen);
    void destroyKey(psa_key_id_t& key);

    bool aesCbcDecrypt(psa_key_id_t key, const uint8_t* iv,
                       const uint8_t* cipher, size_t cipherLen,
                       uint8_t* plain, size_t plainCapacity,
                       size_t& plainLen);

    static bool base64Decode(const std::string& text,
                             std::vector<uint8_t>& out);
    static std::string base64Encode(const uint8_t* data, size_t len);

private:
    AirPlayCrypto() = default;
    ~AirPlayCrypto() = default;
    AirPlayCrypto(const AirPlayCrypto&) = delete;
    AirPlayCrypto& operator=(const AirPlayCrypto&) = delete;

    bool importRsaKeys();
    bool selfTest();
    bool importRsaKey(const uint8_t* der, size_t derLen,
                      psa_key_usage_t usage, psa_algorithm_t alg,
                      psa_key_id_t& out);

    bool _ready = false;
    psa_key_id_t _rsaDecryptKey = 0;
    psa_key_id_t _rsaSignKey = 0;
    std::vector<uint8_t> _aesScratch;
};

} // namespace airplay

#endif // __AIRPLAY_CRYPTO_H__
