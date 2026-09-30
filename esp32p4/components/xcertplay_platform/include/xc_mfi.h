/*
 * MFi authentication coprocessor driver (register protocol over I2C).
 *
 * Ported from xcertplay's MfiAuthenticationClient: a register select is a
 * write-only transaction, the value is read with a separate read transaction.
 * Works with CP2A and similar Apple MFi coprocessors at 0x10/0x11.
 *
 * Registers:
 *   0x02 protocol major        0x30 certificate length (u16 BE)
 *   0x05 error                 0x31.. certificate body (128-byte windows)
 *   0x10 status (write 0x01)   0x20 challenge length (u16 BE)
 *   0x11 response length       0x21 challenge data
 *   0x12 response data         0x10 read 0x10 == success
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_MFI_MAX_CHALLENGE 128
#define XC_MFI_MAX_SIGNATURE 256
#define XC_MFI_MAX_CERTIFICATE 1024

typedef struct {
    bool detected;      /*!< an MFi chip answered on 0x10 or 0x11 */
    uint8_t address;    /*!< its I2C address, valid when detected */
    int device_count;   /*!< total number of I2C devices found on the bus */
} xc_mfi_scan_result_t;

/** Scan the board I2C bus and report MFi coprocessor presence. */
esp_err_t xc_mfi_scan(xc_mfi_scan_result_t *out);

/** Bind the MFi coprocessor at the given 7-bit address on the board I2C bus. */
esp_err_t xc_mfi_open(uint8_t address7bit);

/** Release the MFi device handle. */
void xc_mfi_close(void);

/** Read the protocol-major register (0x02). */
esp_err_t xc_mfi_protocol_major(uint8_t *out_major);

/** Read the certificate length register (0x30). */
esp_err_t xc_mfi_certificate_length(uint16_t *out_length);

/** Read the MFi certificate through 128-byte register windows. */
esp_err_t xc_mfi_read_certificate(uint8_t *buf, size_t cap, size_t *out_len);

/**
 * Sign a 1..128 byte challenge and return the coprocessor signature.
 * This is the operation used during iAP2 0xaa02/0xaa03 and AirPlay /auth-setup.
 */
esp_err_t xc_mfi_sign_challenge(const uint8_t *challenge, size_t len,
                                uint8_t *sig, size_t cap, size_t *out_sig_len);

/** Probe protocol major, certificate length and a sample signature. */
esp_err_t xc_mfi_selftest(void);

/**
 * Cached device certificate (read at boot, before the first sign operation:
 * the coprocessor locks the certificate window once authentication starts).
 * Returns false when not cached yet.
 */
bool xc_mfi_cached_certificate(const uint8_t **cert, size_t *len);

/** Diagnostic: test several register read strategies and log the results. */
void xc_mfi_diag(void);

#ifdef __cplusplus
}
#endif
