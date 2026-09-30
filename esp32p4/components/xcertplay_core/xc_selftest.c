#include "xc_selftest.h"

#include <string.h>

#include "esp_log.h"
#include "xc_crypto.h"
#include "xc_tlv8.h"
#include "xc_bigint.h"

static const char *TAG = "xc_selftest";

static bool hex_eq(const uint8_t *got, const char *want_hex, size_t len)
{
    static const char *digits = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        if (digits[got[i] >> 4] != want_hex[i * 2] || digits[got[i] & 0xf] != want_hex[i * 2 + 1]) {
            return false;
        }
    }
    return want_hex[len * 2] == '\0';
}

static bool test_hkdf(void)
{
    const uint8_t ikm[22] = {
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
    };
    uint8_t salt[13];
    uint8_t info[10];
    for (int i = 0; i < 13; i++) salt[i] = (uint8_t)i;
    for (int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);

    uint8_t okm[42];
    if (xc_hkdf_sha512(salt, sizeof(salt), ikm, sizeof(ikm), info, sizeof(info),
                       okm, sizeof(okm)) != ESP_OK) {
        return false;
    }
    return hex_eq(okm, "832390086cda71fb47625bb5ceb168e4c8e26a1a16ed34d9fc7fe92c14815793"
                       "38da362cb8d9f925d7cb", sizeof(okm));
}

static bool test_chacha20poly1305(void)
{
    const uint8_t key[32] = {
        0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a,
        0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95,
        0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
    };
    const uint8_t nonce[12] = {0x07, 0x00, 0x00, 0x00, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47};
    const uint8_t aad[12] = {0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7};
    const char *pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip "
                     "for the future, sunscreen would be it.";
    size_t pt_len = strlen(pt);

    uint8_t ct[160];
    uint8_t tag[16];
    if (xc_chacha20poly1305_seal(key, nonce, aad, sizeof(aad),
                                 (const uint8_t *)pt, pt_len, ct, tag) != ESP_OK) {
        return false;
    }
    static const char *want_ct =
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
        "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
        "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
        "3ff4def08e4b7a9de576d26586cec64b6116";
    static const char *want_tag = "1ae10b594f09e26a7e902ecbd0600691";
    if (!hex_eq(ct, want_ct, pt_len) || !hex_eq(tag, want_tag, 16)) {
        return false;
    }

    uint8_t roundtrip[160];
    if (xc_chacha20poly1305_open(key, nonce, aad, sizeof(aad), ct, pt_len, tag, roundtrip) != ESP_OK) {
        return false;
    }
    if (memcmp(roundtrip, pt, pt_len) != 0) {
        return false;
    }

    uint8_t bad_tag[16];
    memcpy(bad_tag, tag, 16);
    bad_tag[0] ^= 0xff;
    return xc_chacha20poly1305_open(key, nonce, aad, sizeof(aad), ct, pt_len, bad_tag, roundtrip) ==
           ESP_ERR_INVALID_CRC;
}

static bool test_tlv8(void)
{
    uint8_t buf[384];
    size_t pos = 0;
    uint8_t big[300];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)i;

    if (xc_tlv8_write_u8(buf, sizeof(buf), &pos, 1, 0x42) != ESP_OK) return false;
    if (xc_tlv8_write(buf, sizeof(buf), &pos, 2, big, sizeof(big)) != ESP_OK) return false;

    uint8_t out[300];
    size_t out_len = 0;
    if (xc_tlv8_find_u8(buf, pos, 1, out) != ESP_OK || out[0] != 0x42) return false;
    if (xc_tlv8_find(buf, pos, 2, out, sizeof(out), &out_len) != ESP_OK) return false;
    if (out_len != sizeof(big) || memcmp(out, big, sizeof(big)) != 0) return false;

    /* 300 bytes must fragment into 255 + 45 => 2 physical items for type 2. */
    return xc_tlv8_count(buf, pos) == 3;
}

bool xc_selftest_run(void)
{
    if (xc_crypto_init() != ESP_OK) {
        ESP_LOGE(TAG, "crypto init failed");
        return false;
    }
    bool ok = true;
    ok &= test_hkdf();
    ESP_LOGI(TAG, "hkdf-sha512: %s", ok ? "ok" : "FAIL");
    bool chacha = test_chacha20poly1305();
    ESP_LOGI(TAG, "chacha20-poly1305: %s", chacha ? "ok" : "FAIL");
    bool tlv8 = test_tlv8();
    ESP_LOGI(TAG, "tlv8: %s", tlv8 ? "ok" : "FAIL");
    bool bigint = xc_bigint_selftest();
    ESP_LOGI(TAG, "bigint modexp (3072-bit): %s", bigint ? "ok" : "FAIL");
    xc_asym_selftest();
    return ok && chacha && tlv8 && bigint;
}
