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

/*
 * iAP2-over-CarPlay tunnel (wireless CarPlay handoff).
 *
 * After the phone bootstraps iAP2 over Bluetooth it sends `disableBluetooth`
 * and expects the accessory to move the iAP2 link onto the CarPlay (Wi-Fi)
 * tunnel: link sync frames and CSM messages travel as AirPlay event-channel
 * `iAPSendMessage` commands, mirroring LIVI's bringup. When the tunnel comes
 * up the RFCOMM link is dropped and re-negotiation starts from scratch
 * (identification, MFi, subscriptions run again inside the tunnel).
 */

/** Register the transport that ships link bytes to the phone (event channel). */
void xc_link_set_tunnel_tx(void (*tx)(const uint8_t *data, size_t len));

/** Request the handoff; applied asynchronously by the link task. */
esp_err_t xc_link_enter_tunnel(void);

/** Feed iAP2 link bytes received from the phone over the tunnel. */
void xc_link_tunnel_rx(const uint8_t *data, size_t len);

/** True while the link is running over the CarPlay tunnel. */
bool xc_link_in_tunnel(void);

#ifdef __cplusplus
}
#endif
