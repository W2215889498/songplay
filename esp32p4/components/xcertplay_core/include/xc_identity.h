/*
 * Persistent AirPlay identity and paired-controller store (NVS).
 *
 * The Ed25519 long-term key identifies this accessory across reboots; the
 * controller (iPhone) key is saved by pair-setup and used by pair-verify.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t ed_priv[32];
    uint8_t ed_pub[32];
    char pairing_id[40]; /* UUID string used as the accessory identifier */
} xc_identity_t;

/** Load the identity from NVS, generating and saving it on first boot. */
esp_err_t xc_identity_init(void);

/** Get the process-wide identity (valid after xc_identity_init). */
const xc_identity_t *xc_identity_get(void);

/** Persist a paired controller's long-term public key. */
esp_err_t xc_pairing_save(const char *controller_id, const uint8_t ltpk[32]);

/** Load a paired controller's key; ESP_ERR_NOT_FOUND if it is not the paired one. */
esp_err_t xc_pairing_load(const char *controller_id, uint8_t ltpk[32]);

#ifdef __cplusplus
}
#endif
