#ifndef __HAP_H__
#define __HAP_H__

#include "esp_err.h"
#include <span>
/**
 * HAP (HomeKit Accessory Protocol) implementation for AirPlay 2
 * Handles pair-verify for transient pairing
 */

inline constexpr int HAP_ED25519_PUBLIC_KEY_SIZE = 32;
inline constexpr int HAP_ED25519_SECRET_KEY_SIZE = 64;
inline constexpr int HAP_X25519_KEY_SIZE = 32;
inline constexpr int HAP_CHACHA20_KEY_SIZE = 32;
inline constexpr int HAP_CHACHA20_NONCE_SIZE = 12;
inline constexpr int HAP_POLY1305_TAG_SIZE = 16;


inline constexpr const char* AIRPLAY_NVS_NAMESPACE = "airplay";
inline constexpr const char* AIRPLAY_NVS_KEY_PUBLIC = "ed25519_pub";
inline constexpr const char* AIRPLAY_NVS_KEY_SECRET = "ed25519_sec";

class HAP {
public:

    HAP() = default;
    ~HAP() = default;

    esp_err_t init(void);
    esp_err_t deinit(void);

    std::span<const uint8_t, HAP_ED25519_PUBLIC_KEY_SIZE> getPublicKey() const { return _device_public_key; }
    std::span<const uint8_t, HAP_ED25519_SECRET_KEY_SIZE> getSecretKey() const { return _device_secret_key; }

private:
    constexpr static const char* _TAG = "HAP";

    bool  _initalized = false;

    uint8_t _device_public_key[HAP_ED25519_PUBLIC_KEY_SIZE];
    uint8_t _device_secret_key[HAP_ED25519_SECRET_KEY_SIZE];

    // Ephemeral X25519 keypair for this session
    uint8_t _session_public_key[HAP_X25519_KEY_SIZE];
    uint8_t _session_secret_key[HAP_X25519_KEY_SIZE];

    // Client's ephemeral public key
    uint8_t _client_public_key[HAP_X25519_KEY_SIZE];

    // Shared secret from X25519
    uint8_t _shared_secret[HAP_X25519_KEY_SIZE];

    // Derived session keys
    uint8_t _encrypt_key[HAP_CHACHA20_KEY_SIZE];
    uint8_t _decrypt_key[HAP_CHACHA20_KEY_SIZE];

    // Encryption nonces (counters)
    uint64_t _encrypt_nonce;
    uint64_t _decrypt_nonce;

    // Session state
    int _pair_verify_state;
    int _pair_setup_state;
    bool _pair_setup_transient;
    bool _session_established;

    // SRP session for pair-setup
    struct srp_session *_srp;
};

#endif // __HAP_H__