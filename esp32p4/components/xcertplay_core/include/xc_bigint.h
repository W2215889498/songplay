/*
 * Fixed-size (3072-bit / 384-byte) modular arithmetic for SRP-6a pairing.
 *
 * mbedTLS 4 keeps its bignum private, so this is a self-contained Montgomery
 * implementation sized for the RFC 5054 3072-bit group used by AirPlay
 * pair-setup. Only a handful of modexps run per pairing, so speed is not critical.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_BN_BYTES 384

esp_err_t xc_modexp(const uint8_t *base, size_t base_len,
                    const uint8_t *exp, size_t exp_len,
                    const uint8_t mod[XC_BN_BYTES],
                    uint8_t out[XC_BN_BYTES]);

esp_err_t xc_modmul(const uint8_t a[XC_BN_BYTES], const uint8_t b[XC_BN_BYTES],
                    const uint8_t mod[XC_BN_BYTES], uint8_t out[XC_BN_BYTES]);

esp_err_t xc_modadd(const uint8_t a[XC_BN_BYTES], const uint8_t b[XC_BN_BYTES],
                    const uint8_t mod[XC_BN_BYTES], uint8_t out[XC_BN_BYTES]);

bool xc_bigint_selftest(void);

#ifdef __cplusplus
}
#endif
