/*
 * SRP-6a server (RFC 5054 3072-bit group, SHA-512) for AirPlay pair-setup.
 *
 * The username is "Pair-Setup" and the password is the fixed code "3939", as
 * specified by the AirPlay pairing protocol.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "xc_bigint.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t salt[16];
    uint8_t B[XC_BN_BYTES];
    uint8_t b[32];
    uint8_t verifier[XC_BN_BYTES];
    char identifier[32];
    size_t identifier_len;
} xc_srp_session_t;

esp_err_t xc_srp_start(xc_srp_session_t *s, const char *username, const char *password);

esp_err_t xc_srp_verify(xc_srp_session_t *s, const uint8_t *public_a, size_t public_a_len,
                        const uint8_t *client_proof, size_t proof_len,
                        uint8_t session_key[64], uint8_t server_proof[64]);

void xc_srp_free(xc_srp_session_t *s);

#ifdef __cplusplus
}
#endif
