#include "AirPlayCrypto.h"

#include <cstring>

#include "AirPlayTypes.h"

#include "esp_log.h"
#include "mbedtls/pk.h"
#include "sodium.h"

namespace airplay {

namespace {

constexpr const char* TAG = "AirPlayCrypto";

// The classic AirPlay/RAOP RSA keypair. Clients encrypt the per-session
// AES key with the matching public key, so a receiver has to use the
// protocol's well-known key to interoperate with AirPlay 1 senders.
constexpr const char* kRsaPrivateKeyPem = R"PEM(-----BEGIN RSA PRIVATE KEY-----
MIIEpQIBAAKCAQEA59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Q
g90u3sG/1CUtwC5vOYvfDmFI6oSFXi5ELabWJmT2dKHzBJKa3k9ok+8t9ucRqMd6
DZHJ2YCCLlDRKSKv6kDqnw4UwPdpOMXziC/AMj3Z/lUVX1G7WSHCAWKf1zNS1eLv
qr+boEjXuBOitnZ/bDzPHrTOZz0Dew0uowxf/+sG+NCK3eQJVxqcaJ/vEHKIVd2M
+5qL71yJQ+87X6oV3eaYvt3zWZYD6z5vYTcrtij2VZ9Zmni/UAaHqn9JdsBWLUEp
VviYnhimNVvYFZeCXg/IdTQ+x4IRdiXNv5hEewIDAQABAoIBAQDl8Axy9XfWBLmk
zkEiqoSwF0PsmVrPzH9KsnwLGH+QZlvjWd8SWYGN7u1507HvhF5N3drJoVU3O14n
DY4TFQAaLlJ9VM35AApXaLyY1ERrN7u9ALKd2LUwYhM7Km539O4yUFYikE2nIPsc
EsA5ltpxOgUGCY7b7ez5NtD6nL1ZKauw7aNXmVAvmJTcuPxWmoktF3gDJKK2wxZu
NGcJE0uFQEG4Z3BrWP7yoNuSK3dii2jmlpPHr0O/KnPQtzI3eguhe0TwUem/eYSd
yzMyVx/YpwkzwtYL3sR5k0o9rKQLtvLzfAqdBxBurcizaaA/L0HIgAmOit1GJA2s
aMxTVPNhAoGBAPfgv1oeZxgxmotiCcMXFEQEWflzhWYTsXrhUIuz5jFua39GLS99
ZEErhLdrwj8rDDViRVJ5skOp9zFvlYAHs0xh92ji1E7V/ysnKBfsMrPkk5KSKPrn
jndMoPdevWnVkgJ5jxFuNgxkOLMuG9i53B4yMvDTCRiIPMQ++N2iLDaRAoGBAO9v
//mU8eVkQaoANf0ZoMjW8CN4xwWA2cSEIHkd9AfFkftuv8oyLDCG3ZAf0vrhrrtk
rfa7ef+AUb69DNggq4mHQAYBp7L+k5DKzJrKuO0r+R0YbY9pZD1+/g9dVt91d6LQ
NepUE/yY2PP5CNoFmjedpLHMOPFdVgqDzDFxU8hLAoGBANDrr7xAJbqBjHVwIzQ4
To9pb4BNeqDndk5Qe7fT3+/H1njGaC0/rXE0Qb7q5ySgnsCb3DvAcJyRM9SJ7OKl
Gt0FMSdJD5KG0XPIpAVNwgpXXH5MDJg09KHeh0kXo+QA6viFBi21y340NonnEfdf
54PX4ZGS/Xac1UK+pLkBB+zRAoGAf0AY3H3qKS2lMEI4bzEFoHeK3G895pDaK3TF
BVmD7fV0Zhov17fegFPMwOII8MisYm9ZfT2Z0s5Ro3s5rkt+nvLAdfC/PYPKzTLa
lpGSwomSNYJcB9HNMlmhkGzc1JnLYT4iyUyx6pcZBmCd8bD0iwY/FzcgNDaUmbX9
+XDvRA0CgYEAkE7pIPlE71qvfJQgoA9em0gILAuE4Pu13aKiJnfft7hIjbK+5kyb
3TysZvoyDnb3HOKvInK7vXbKuU4ISgxB2bB3HcYzQMGsz1qJ2gG0N5hvJpzwwhbh
XqFKA4zaaSrw622wDniAK5MlIE0tIAKKP4yxNGjoD2QYjhBGuhvkWKY=
-----END RSA PRIVATE KEY-----
)PEM";

} // namespace

AirPlayCrypto& AirPlayCrypto::instance()
{
    static AirPlayCrypto crypto;
    return crypto;
}

esp_err_t AirPlayCrypto::init()
{
    if (_ready) {
        return ESP_OK;
    }

    if (sodium_init() < 0) {
        ESP_LOGE(TAG, "libsodium init failed");
        return ESP_FAIL;
    }

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", static_cast<int>(status));
        return ESP_FAIL;
    }

    if (!importRsaKeys()) {
        return ESP_FAIL;
    }
    if (!selfTest()) {
        return ESP_FAIL;
    }

    _aesScratch.resize(16 + kMaxRtpPacketSize);
    _ready = true;
    ESP_LOGI(TAG, "RSA/AES crypto ready");
    return ESP_OK;
}

bool AirPlayCrypto::importRsaKeys()
{
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    int ret = mbedtls_pk_parse_key(
        &pk, reinterpret_cast<const unsigned char*>(kRsaPrivateKeyPem),
        std::strlen(kRsaPrivateKeyPem) + 1, nullptr, 0);
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_pk_parse_key failed: -0x%04x", -ret);
        mbedtls_pk_free(&pk);
        return false;
    }

    std::vector<uint8_t> der(2048);
    int derLen = mbedtls_pk_write_key_der(&pk, der.data(), der.size());
    mbedtls_pk_free(&pk);
    if (derLen <= 0 || static_cast<size_t>(derLen) > der.size()) {
        ESP_LOGE(TAG, "mbedtls_pk_write_key_der failed: -0x%04x", -derLen);
        return false;
    }

    // mbedtls writes DER at the end of the buffer.
    const uint8_t* derPtr = der.data() + der.size() - static_cast<size_t>(derLen);

    if (!importRsaKey(derPtr, static_cast<size_t>(derLen),
                      PSA_KEY_USAGE_DECRYPT | PSA_KEY_USAGE_EXPORT,
                      PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1), _rsaDecryptKey)) {
        return false;
    }
    if (!importRsaKey(derPtr, static_cast<size_t>(derLen),
                      PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT,
                      PSA_ALG_RSA_PKCS1V15_SIGN_RAW, _rsaSignKey)) {
        return false;
    }

    return true;
}

bool AirPlayCrypto::selfTest()
{
    std::vector<uint8_t> publicKey(512);
    size_t publicKeyLen = 0;
    psa_status_t status = psa_export_public_key(
        _rsaDecryptKey, publicKey.data(), publicKey.size(), &publicKeyLen);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Self-test: export public key failed: %d",
                 static_cast<int>(status));
        return false;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_RSA_PUBLIC_KEY);
    psa_set_key_bits(&attributes, 2048);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1));

    psa_key_id_t encryptKey = 0;
    status = psa_import_key(&attributes, publicKey.data(), publicKeyLen,
                            &encryptKey);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Self-test: import public key failed: %d",
                 static_cast<int>(status));
        return false;
    }

    const uint8_t plain[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    };
    std::vector<uint8_t> cipher(256);
    size_t cipherLen = 0;
    status = psa_asymmetric_encrypt(
        encryptKey, PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1), plain, sizeof(plain),
        nullptr, 0, cipher.data(), cipher.size(), &cipherLen);
    destroyKey(encryptKey);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Self-test: OAEP encrypt failed: %d",
                 static_cast<int>(status));
        return false;
    }

    std::vector<uint8_t> decrypted(64);
    size_t decryptedLen = 0;
    status = psa_asymmetric_decrypt(
        _rsaDecryptKey, PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1), cipher.data(),
        cipherLen, nullptr, 0, decrypted.data(), decrypted.size(),
        &decryptedLen);
    if (status != PSA_SUCCESS || decryptedLen != sizeof(plain) ||
        std::memcmp(decrypted.data(), plain, sizeof(plain)) != 0) {
        ESP_LOGE(TAG, "Self-test: OAEP decrypt failed: %d",
                 static_cast<int>(status));
        return false;
    }

    const uint8_t message[26] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a,
    };
    std::vector<uint8_t> signature(256);
    size_t signatureLen = 0;
    status = psa_sign_hash(
        _rsaSignKey, PSA_ALG_RSA_PKCS1V15_SIGN_RAW, message,
        sizeof(message), signature.data(), signature.size(), &signatureLen);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Self-test: RSA sign failed: %d",
                 static_cast<int>(status));
        return false;
    }

    psa_set_key_type(&attributes, PSA_KEY_TYPE_RSA_PUBLIC_KEY);
    psa_set_key_bits(&attributes, 2048);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_RSA_PKCS1V15_SIGN_RAW);
    psa_key_id_t verifyKey = 0;
    status = psa_import_key(&attributes, publicKey.data(), publicKeyLen,
                            &verifyKey);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Self-test: import verify key failed: %d",
                 static_cast<int>(status));
        return false;
    }
    status = psa_verify_hash(
        verifyKey, PSA_ALG_RSA_PKCS1V15_SIGN_RAW, message, sizeof(message),
        signature.data(), signatureLen);
    destroyKey(verifyKey);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Self-test: RSA verify failed: %d",
                 static_cast<int>(status));
        return false;
    }

    ESP_LOGI(TAG, "RSA self-test passed");
    return true;
}

bool AirPlayCrypto::importRsaKey(const uint8_t* der, size_t derLen,
                                 psa_key_usage_t usage, psa_algorithm_t alg,
                                 psa_key_id_t& out)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_RSA_KEY_PAIR);
    psa_set_key_bits(&attr, 2048);
    psa_set_key_usage_flags(&attr, usage);
    psa_set_key_algorithm(&attr, alg);

    psa_status_t status = psa_import_key(&attr, der, derLen, &out);
    psa_reset_key_attributes(&attr);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key failed: %d", static_cast<int>(status));
        out = 0;
        return false;
    }
    return true;
}

std::string AirPlayCrypto::appleResponse(const uint8_t* challenge,
                                         size_t challengeLen, uint32_t ipv4,
                                         const uint8_t mac[6])
{
    if (!_ready || challenge == nullptr || challengeLen == 0) {
        return {};
    }

    uint8_t message[64];
    if (challengeLen + 4 + 6 > sizeof(message)) {
        return {};
    }

    std::memcpy(message, challenge, challengeLen);
    std::memcpy(message + challengeLen, &ipv4, sizeof(ipv4));
    std::memcpy(message + challengeLen + sizeof(ipv4), mac, 6);
    size_t messageLen = challengeLen + sizeof(ipv4) + 6;

    uint8_t signature[256];
    size_t signatureLen = 0;
    psa_status_t status = psa_sign_hash(
        _rsaSignKey, PSA_ALG_RSA_PKCS1V15_SIGN_RAW, message, messageLen,
        signature, sizeof(signature), &signatureLen);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Apple-Challenge sign failed: %d",
                 static_cast<int>(status));
        return {};
    }

    return base64Encode(signature, signatureLen);
}

bool AirPlayCrypto::decryptAesKey(const uint8_t* encrypted,
                                  size_t encryptedLen,
                                  std::vector<uint8_t>& aesKey)
{
    if (!_ready || encrypted == nullptr || encryptedLen == 0) {
        return false;
    }

    uint8_t output[64];
    size_t outputLen = 0;
    psa_status_t status = psa_asymmetric_decrypt(
        _rsaDecryptKey, PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1), encrypted,
        encryptedLen, nullptr, 0, output, sizeof(output), &outputLen);
    if (status != PSA_SUCCESS || outputLen < 16) {
        ESP_LOGE(TAG, "rsaaeskey decrypt failed: %d", static_cast<int>(status));
        return false;
    }

    aesKey.assign(output, output + outputLen);
    return true;
}

psa_key_id_t AirPlayCrypto::importAesKey(const uint8_t* key, size_t keyLen)
{
    if (key == nullptr || (keyLen != 16 && keyLen != 32)) {
        return 0;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, static_cast<size_t>(keyLen) * 8);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_CBC_NO_PADDING);

    psa_key_id_t keyId = 0;
    psa_status_t status = psa_import_key(&attr, key, keyLen, &keyId);
    psa_reset_key_attributes(&attr);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "AES key import failed: %d", static_cast<int>(status));
        return 0;
    }
    return keyId;
}

void AirPlayCrypto::destroyKey(psa_key_id_t& key)
{
    if (key != 0) {
        psa_destroy_key(key);
        key = 0;
    }
}

bool AirPlayCrypto::aesCbcDecrypt(psa_key_id_t key, const uint8_t* iv,
                                  const uint8_t* cipher, size_t cipherLen,
                                  uint8_t* plain, size_t plainCapacity,
                                  size_t& plainLen)
{
    plainLen = 0;
    if (key == 0 || iv == nullptr || cipher == nullptr || plain == nullptr) {
        return false;
    }
    if (cipherLen == 0) {
        return true;
    }

    size_t needed = 16 + cipherLen;
    if (needed > _aesScratch.size()) {
        _aesScratch.resize(needed);
    }

    std::memcpy(_aesScratch.data(), iv, 16);
    std::memcpy(_aesScratch.data() + 16, cipher, cipherLen);

    size_t outLen = 0;
    psa_status_t status = psa_cipher_decrypt(
        key, PSA_ALG_CBC_NO_PADDING, _aesScratch.data(), needed, plain,
        plainCapacity, &outLen);
    if (status != PSA_SUCCESS) {
        ESP_LOGW(TAG, "AES-CBC decrypt failed: %d", static_cast<int>(status));
        return false;
    }

    plainLen = outLen;
    return true;
}

bool AirPlayCrypto::base64Decode(const std::string& text,
                                 std::vector<uint8_t>& out)
{
    out.clear();
    if (text.empty()) {
        return true;
    }

    size_t capacity = (text.size() * 3) / 4 + 4;
    out.resize(capacity);
    size_t outLen = 0;

    if (sodium_base642bin(out.data(), out.size(), text.c_str(), text.size(),
                          " \t\r\n", &outLen, nullptr,
                          sodium_base64_VARIANT_ORIGINAL_NO_PADDING) != 0) {
        if (sodium_base642bin(out.data(), out.size(), text.c_str(), text.size(),
                              " \t\r\n", &outLen, nullptr,
                              sodium_base64_VARIANT_ORIGINAL) != 0) {
            out.clear();
            return false;
        }
    }

    out.resize(outLen);
    return true;
}

std::string AirPlayCrypto::base64Encode(const uint8_t* data, size_t len)
{
    size_t encodedLen = sodium_base64_ENCODED_LEN(
        len, sodium_base64_VARIANT_ORIGINAL);
    std::string result(encodedLen, '\0');
    sodium_bin2base64(result.data(), result.size(), data, len,
                      sodium_base64_VARIANT_ORIGINAL);
    result.resize(std::strlen(result.c_str()));
    return result;
}

} // namespace airplay
