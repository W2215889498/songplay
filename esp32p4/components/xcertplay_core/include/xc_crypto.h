/*
 * Cryptographic primitives shared by the CarPlay protocol stack.
 *
 * Implemented on top of the PSA Crypto API (mbedTLS 4.x in ESP-IDF 6.x).
 * These map to the primitives used by xcertplay's AirPlay/iAP2 layer:
 * HKDF-SHA512, ChaCha20-Poly1305, AES-128-CTR, SHA-512 and a CSPRNG.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_SHA512_LEN 64
#define XC_CHACHA20_KEY_LEN 32
#define XC_CHACHA20_NONCE_LEN 12
#define XC_POLY1305_TAG_LEN 16
#define XC_AES128_KEY_LEN 16

/** One-time crypto subsystem init (idempotent). */
esp_err_t xc_crypto_init(void);

/** Fill a buffer with cryptographically secure random bytes. */
esp_err_t xc_random(void *out, size_t len);

/** SHA-512 digest. */
esp_err_t xc_sha512(const uint8_t *in, size_t in_len, uint8_t out[XC_SHA512_LEN]);

/** SHA-256 digest (CarPlay /auth-setup, MFi protocol major 3). */
esp_err_t xc_sha256(const uint8_t *in, size_t in_len, uint8_t out[32]);

/** SHA-1 digest (/auth-setup key derivation). */
esp_err_t xc_sha1(const uint8_t *in, size_t in_len, uint8_t out[20]);

/**
 * HKDF-SHA512 extract-and-expand (RFC 5869).
 * Any of salt/info may be NULL when its length is 0.
 */
esp_err_t xc_hkdf_sha512(const uint8_t *salt, size_t salt_len,
                         const uint8_t *ikm, size_t ikm_len,
                         const uint8_t *info, size_t info_len,
                         uint8_t *out, size_t out_len);

/**
 * ChaCha20-Poly1305 AEAD seal.
 * Output is written as a separate ciphertext buffer and a 16-byte tag, which
 * matches the AirPlay frames (2-byte AAD length prefix + ciphertext + tag).
 */
esp_err_t xc_chacha20poly1305_seal(const uint8_t key[XC_CHACHA20_KEY_LEN],
                                   const uint8_t nonce[XC_CHACHA20_NONCE_LEN],
                                   const uint8_t *aad, size_t aad_len,
                                   const uint8_t *pt, size_t pt_len,
                                   uint8_t *ct, uint8_t tag[XC_POLY1305_TAG_LEN]);

/** ChaCha20-Poly1305 AEAD open; returns ESP_ERR_INVALID_CRC on tag mismatch. */
esp_err_t xc_chacha20poly1305_open(const uint8_t key[XC_CHACHA20_KEY_LEN],
                                   const uint8_t nonce[XC_CHACHA20_NONCE_LEN],
                                   const uint8_t *aad, size_t aad_len,
                                   const uint8_t *ct, size_t ct_len,
                                   const uint8_t tag[XC_POLY1305_TAG_LEN],
                                   uint8_t *pt);

/** AES-128-CTR keystream (encrypt == decrypt). */
esp_err_t xc_aes128_ctr(const uint8_t key[XC_AES128_KEY_LEN],
                        const uint8_t iv[16],
                        const uint8_t *in, uint8_t *out, size_t len);

#define XC_CURVE25519_LEN 32
#define XC_ED25519_SIG_LEN 64

/** AirPlay nonce: four zero bytes followed by the 8-byte ASCII label. */
esp_err_t xc_nonce_label(const char *label, uint8_t nonce[XC_CHACHA20_NONCE_LEN]);

/** Generate an ephemeral X25519 key pair. */
esp_err_t xc_x25519_generate(uint8_t priv[XC_CURVE25519_LEN], uint8_t pub[XC_CURVE25519_LEN]);

/** X25519 ECDH shared secret. */
esp_err_t xc_x25519_shared(const uint8_t priv[XC_CURVE25519_LEN],
                           const uint8_t peer_pub[XC_CURVE25519_LEN],
                           uint8_t out[XC_CURVE25519_LEN]);

/** Derive the Ed25519 public key from a 32-byte seed (private key). */
esp_err_t xc_ed25519_public_from_private(const uint8_t priv[32], uint8_t pub[32]);

/** Ed25519 signature over an arbitrary message. */
esp_err_t xc_ed25519_sign(const uint8_t priv[32], const uint8_t *msg, size_t len,
                          uint8_t sig[XC_ED25519_SIG_LEN]);

/** Ed25519 verify; ESP_ERR_INVALID_CRC when the signature is bad. */
esp_err_t xc_ed25519_verify(const uint8_t pub[32], const uint8_t *msg, size_t len,
                            const uint8_t sig[XC_ED25519_SIG_LEN]);

/** Diagnostic self-test for X25519/Ed25519; logs each step. */
void xc_asym_selftest(void);

#ifdef __cplusplus
}
#endif
