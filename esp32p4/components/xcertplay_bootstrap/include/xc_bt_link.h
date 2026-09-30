/*
 * iAP2 link layer over the ESP32 classic-Bluetooth bridge.
 *
 * The bridge (xcertplay-bt-bridge) exposes the iPhone's iAP2 RFCOMM stream as a
 * simple framed UART protocol:
 *     A5 | type | len_lo | len_hi | payload      (type 1 = iAP2, 3 = text event)
 * This module drives the iAP2 link protocol (marker, synchronization, sequence
 * numbers, acknowledgements), reassembles the CSM layer (40 40 frames) and
 * exposes send/receive of whole CSM messages.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called for every complete CSM message received from the phone. */
typedef void (*xc_csm_rx_cb_t)(uint16_t msg_id, const uint8_t *body, size_t len);

/** Start the UART link to the bridge and the iAP2 link state machine. */
esp_err_t xc_link_start(xc_csm_rx_cb_t on_message);

/** True once the link negotiation finished and sending is allowed. */
bool xc_link_writable(void);

/** Queue one CSM message (split across link packets as needed). */
esp_err_t xc_link_send_csm(uint16_t msg_id, const uint8_t *body, size_t len);

/** True when the RFCOMM channel to the phone is up (from bridge events). */
bool xc_link_rfcomm_up(void);

#ifdef __cplusplus
}
#endif
