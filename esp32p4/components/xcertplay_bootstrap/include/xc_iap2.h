/*
 * iAP2 wireless CarPlay bootstrap.
 *
 * Runs the accessory side of the iAP2 exchange that makes an iPhone join our
 * Wi-Fi hotspot: identification (0x1D01), MFi authentication (0xAA01/0xAA03),
 * the Wi-Fi configuration reply (0x5703) and CarPlayStartSession (0x4301).
 * Everything is driven by messages from the phone, exactly like xcertplay.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the bootstrap state machine (needs the BT bridge on UART1). */
esp_err_t xc_iap2_start(void);

#ifdef __cplusplus
}
#endif
