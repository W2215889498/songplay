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

/*
 * CallStateUpdate (0x4155) notifications from the phone. `status`:
 * 0 disconnected, 1 sending, 2 ringing, 3 connecting, 4 active, 5 held,
 * 6 disconnecting. `direction`: 0 unknown, 1 incoming, 2 outgoing.
 * `service`: 0 unknown, 1 telephony, 2 FaceTime audio, 3 FaceTime video.
 * `name`/`number` may be empty strings.
 */
typedef void (*xc_call_state_cb_t)(int status, int direction, int service, const char *name,
                                   const char *number);

/** Register the receiver for call-state notifications (single consumer). */
void xc_iap2_set_call_state_cb(xc_call_state_cb_t cb);

/**
 * Tell the bootstrap that the Wi-Fi AP is up. Plan A brings iAP2 up before the
 * AP; a CarPlayStartSession that arrived earlier is sent on this call.
 */
void xc_iap2_notify_ap_ready(void);

#ifdef __cplusplus
}
#endif
