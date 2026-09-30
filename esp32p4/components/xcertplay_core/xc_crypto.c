#include "xc_crypto.h"

#include <string.h>

#include "psa/crypto.h"
#include "sodium.h"
#include "esp_log.h"

static const char *TAG = "xc_crypto";
static bool s_inited;

esp_err_t xc_crypto_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)st);
        return ESP_FAIL;
    }
    if (sodium_init() < 0) {
        ESP_LOGE(TAG, "sodium_init failed");
        return ESP_FAIL;
    }
    s_inited = true;
    return ESP_OK;
}

static esp_err_t map_status(psa_status_t st, const char *what)
{
    if (st == PSA_SUCCESS) {
        return ESP_OK;
    }
    if (st == PSA_ERROR_INVALID_SIGNATURE) {
        return ESP_ERR_INVALID_CRC;
    }
    ESP_LOGE(TAG, "%s failed: psa_status=%d", what, (int)st);
    return ESP_FAIL;
}

esp_err_t xc_random(void *out, size_t len)
{
    return map_status(psa_generate_random((uint8_t *)out, len), "psa_generate_random");
}

esp_err_t xc_sha512(const uint8_t *in, size_t in_len, uint8_t out[XC_SHA512_LEN])
{
    size_t olen = 0;
    return map_status(psa_hash_compute(PSA_ALG_SHA_512, in, in_len, out, XC_SHA512_LEN, &olen),
                      "psa_hash_compute");
}

esp_err_t xc_sha256(const uint8_t *in, size_t in_len, uint8_t out[32])
{
    size_t olen = 0;
    return map_status(psa_hash_compute(PSA_ALG_SHA_256, in, in_len, out, 32, &olen),
                      "psa_hash_compute sha256");
}

esp_err_t xc_sha1(const uint8_t *in, size_t in_len, uint8_t out[20])
{
    size_t olen = 0;
    return map_status(psa_hash_compute(PSA_ALG_SHA_1, in, in_len, out, 20, &olen),
                      "psa_hash_compute sha1");
}

esp_err_t xc_hkdf_sha512(const uint8_t *salt, size_t salt_len,
                         const uint8_t *ikm, size_t ikm_len,
                         const uint8_t *info, size_t info_len,
                         uint8_t *out, size_t out_len)
{
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t st = psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_512));

    if (st == PSA_SUCCESS) {
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_len);
    }
    if (st == PSA_SUCCESS) {
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SECRET, ikm, ikm_len);
    }
    if (st == PSA_SUCCESS) {
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO, info, info_len);
    }
    if (st == PSA_SUCCESS) {
        st = psa_key_derivation_output_bytes(&op, out, out_len);
    }
    psa_key_derivation_abort(&op);
    return map_status(st, "hkdf");
}

static esp_err_t import_aead_key(const uint8_t *key, psa_key_usage_t usage, psa_key_id_t *out)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_CHACHA20);
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, usage);
    psa_set_key_algorithm(&attr, PSA_ALG_CHACHA20_POLY1305);
    return map_status(psa_import_key(&attr, key, XC_CHACHA20_KEY_LEN, out), "import chacha key");
}

esp_err_t xc_chacha20poly1305_seal(const uint8_t key[XC_CHACHA20_KEY_LEN],
                                   const uint8_t nonce[XC_CHACHA20_NONCE_LEN],
                                   const uint8_t *aad, size_t aad_len,
                                   const uint8_t *pt, size_t pt_len,
                                   uint8_t *ct, uint8_t tag[XC_POLY1305_TAG_LEN])
{
    psa_key_id_t key_id = 0;
    esp_err_t err = import_aead_key(key, PSA_KEY_USAGE_ENCRYPT, &key_id);
    if (err != ESP_OK) {
        return err;
    }

    psa_aead_operation_t op = PSA_AEAD_OPERATION_INIT;
    psa_status_t st = psa_aead_encrypt_setup(&op, key_id, PSA_ALG_CHACHA20_POLY1305);
    if (st == PSA_SUCCESS) {
        st = psa_aead_set_nonce(&op, nonce, XC_CHACHA20_NONCE_LEN);
    }
    if (st == PSA_SUCCESS) {
        st = psa_aead_set_lengths(&op, aad_len, pt_len);
    }
    if (st == PSA_SUCCESS && aad_len > 0) {
        st = psa_aead_update_ad(&op, aad, aad_len);
    }
    size_t written = 0;
    if (st == PSA_SUCCESS && pt_len > 0) {
        st = psa_aead_update(&op, pt, pt_len, ct, pt_len, &written);
    }
    size_t tag_len = 0;
    if (st == PSA_SUCCESS) {
        st = psa_aead_finish(&op, ct + written, pt_len - written, &tag_len, tag,
                             XC_POLY1305_TAG_LEN, &tag_len);
    }
    psa_aead_abort(&op);
    psa_destroy_key(key_id);
    return map_status(st, "chacha seal");
}

esp_err_t xc_chacha20poly1305_open(const uint8_t key[XC_CHACHA20_KEY_LEN],
                                   const uint8_t nonce[XC_CHACHA20_NONCE_LEN],
                                   const uint8_t *aad, size_t aad_len,
                                   const uint8_t *ct, size_t ct_len,
                                   const uint8_t tag[XC_POLY1305_TAG_LEN],
                                   uint8_t *pt)
{
    psa_key_id_t key_id = 0;
    esp_err_t err = import_aead_key(key, PSA_KEY_USAGE_DECRYPT, &key_id);
    if (err != ESP_OK) {
        return err;
    }

    psa_aead_operation_t op = PSA_AEAD_OPERATION_INIT;
    psa_status_t st = psa_aead_decrypt_setup(&op, key_id, PSA_ALG_CHACHA20_POLY1305);
    if (st == PSA_SUCCESS) {
        st = psa_aead_set_nonce(&op, nonce, XC_CHACHA20_NONCE_LEN);
    }
    if (st == PSA_SUCCESS) {
        st = psa_aead_set_lengths(&op, aad_len, ct_len);
    }
    if (st == PSA_SUCCESS && aad_len > 0) {
        st = psa_aead_update_ad(&op, aad, aad_len);
    }
    size_t written = 0;
    if (st == PSA_SUCCESS && ct_len > 0) {
        st = psa_aead_update(&op, ct, ct_len, pt, ct_len, &written);
    }
    if (st == PSA_SUCCESS) {
        size_t plen = 0;
        st = psa_aead_verify(&op, NULL, 0, &plen, tag, XC_POLY1305_TAG_LEN);
    }
    psa_aead_abort(&op);
    psa_destroy_key(key_id);
    return map_status(st, "chacha open");
}

esp_err_t xc_aes128_ctr(const uint8_t key[XC_AES128_KEY_LEN],
                        const uint8_t iv[16],
                        const uint8_t *in, uint8_t *out, size_t len)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_CTR);

    psa_key_id_t key_id = 0;
    psa_status_t st = psa_import_key(&attr, key, XC_AES128_KEY_LEN, &key_id);
    if (st != PSA_SUCCESS) {
        return map_status(st, "import aes key");
    }

    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    st = psa_cipher_encrypt_setup(&op, key_id, PSA_ALG_CTR);
    if (st == PSA_SUCCESS) {
        st = psa_cipher_set_iv(&op, iv, 16);
    }
    size_t written = 0;
    if (st == PSA_SUCCESS && len > 0) {
        st = psa_cipher_update(&op, in, len, out, len, &written);
    }
    if (st == PSA_SUCCESS) {
        size_t fin = 0;
        st = psa_cipher_finish(&op, out + written, len - written, &fin);
    }
    psa_cipher_abort(&op);
    psa_destroy_key(key_id);
    return map_status(st, "aes-ctr");
}

/* ---- AirPlay asymmetric primitives ------------------------------------ */

#if defined(PSA_ALG_PURE_EDDSA)
#define XC_ED_ALG PSA_ALG_PURE_EDDSA
#else
#define XC_ED_ALG PSA_ALG_EDDSA
#endif

esp_err_t xc_nonce_label(const char *label, uint8_t nonce[XC_CHACHA20_NONCE_LEN])
{
    memset(nonce, 0, XC_CHACHA20_NONCE_LEN);
    size_t n = strlen(label);
    if (n > 8) {
        n = 8;
    }
    memcpy(nonce + 4, label, n);
    return ESP_OK;
}

esp_err_t xc_x25519_generate(uint8_t priv[XC_CURVE25519_LEN], uint8_t pub[XC_CURVE25519_LEN])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&attr, 255);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDH);

    psa_key_id_t key = 0;
    psa_status_t st = psa_generate_key(&attr, &key);
    size_t olen = 0;
    if (st == PSA_SUCCESS) {
        st = psa_export_key(key, priv, XC_CURVE25519_LEN, &olen);
    }
    if (st == PSA_SUCCESS) {
        st = psa_export_public_key(key, pub, XC_CURVE25519_LEN, &olen);
    }
    psa_destroy_key(key);
    return map_status(st, "x25519 generate");
}

esp_err_t xc_x25519_shared(const uint8_t priv[XC_CURVE25519_LEN],
                           const uint8_t peer_pub[XC_CURVE25519_LEN],
                           uint8_t out[XC_CURVE25519_LEN])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&attr, 255);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDH);

    psa_key_id_t key = 0;
    psa_status_t st = psa_import_key(&attr, priv, XC_CURVE25519_LEN, &key);
    size_t olen = 0;
    if (st == PSA_SUCCESS) {
        st = psa_raw_key_agreement(PSA_ALG_ECDH, key, peer_pub, XC_CURVE25519_LEN,
                                   out, XC_CURVE25519_LEN, &olen);
    }
    psa_destroy_key(key);
    return map_status(st, "x25519 ecdh");
}

/* Ed25519 via libsodium (IDF's mbedTLS does not wire Ed25519 into PSA). */
static esp_err_t ed25519_seed_keypair(const uint8_t priv[32], uint8_t pub[32], uint8_t sk[64])
{
    if (sodium_init() < 0) {
        return ESP_FAIL;
    }
    crypto_sign_ed25519_seed_keypair(pub, sk, priv);
    return ESP_OK;
}

esp_err_t xc_ed25519_public_from_private(const uint8_t priv[32], uint8_t pub[32])
{
    uint8_t sk[crypto_sign_ed25519_SECRETKEYBYTES];
    esp_err_t e = ed25519_seed_keypair(priv, pub, sk);
    sodium_memzero(sk, sizeof(sk));
    return e;
}

esp_err_t xc_ed25519_sign(const uint8_t priv[32], const uint8_t *msg, size_t len,
                          uint8_t sig[XC_ED25519_SIG_LEN])
{
    uint8_t pk[crypto_sign_ed25519_PUBLICKEYBYTES];
    uint8_t sk[crypto_sign_ed25519_SECRETKEYBYTES];
    esp_err_t e = ed25519_seed_keypair(priv, pk, sk);
    if (e != ESP_OK) {
        return e;
    }
    unsigned long long siglen = 0;
    crypto_sign_ed25519_detached(sig, &siglen, msg, (unsigned long long)len, sk);
    sodium_memzero(sk, sizeof(sk));
    return siglen == XC_ED25519_SIG_LEN ? ESP_OK : ESP_FAIL;
}

esp_err_t xc_ed25519_verify(const uint8_t pub[32], const uint8_t *msg, size_t len,
                            const uint8_t sig[XC_ED25519_SIG_LEN])
{
    if (sodium_init() < 0) {
        return ESP_FAIL;
    }
    if (crypto_sign_ed25519_verify_detached(sig, msg, (unsigned long long)len, pub) != 0) {
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

void xc_asym_selftest(void)
{
    uint8_t priv[32], pub[32], sig[64], shared[32];
    esp_err_t e;

    e = xc_x25519_generate(priv, pub);
    ESP_LOGI(TAG, "x25519 generate: %s", esp_err_to_name(e));
    if (e == ESP_OK) {
        uint8_t p2[32], s2[32];
        if (xc_x25519_generate(p2, s2) == ESP_OK) {
            e = xc_x25519_shared(priv, s2, shared);
            ESP_LOGI(TAG, "x25519 shared: %s", esp_err_to_name(e));
        }
    }

    uint8_t seed[32];
    memset(seed, 7, 32);
    e = xc_ed25519_public_from_private(seed, pub);
    ESP_LOGI(TAG, "ed25519 public: %s", esp_err_to_name(e));
    if (e == ESP_OK) {
        e = xc_ed25519_sign(seed, seed, 32, sig);
        ESP_LOGI(TAG, "ed25519 sign: %s", esp_err_to_name(e));
        if (e == ESP_OK) {
            e = xc_ed25519_verify(pub, seed, 32, sig);
            ESP_LOGI(TAG, "ed25519 verify: %s", esp_err_to_name(e));
        }
    }
}
