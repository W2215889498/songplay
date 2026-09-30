/*
 * AirPlay RTSP endpoint (port 7000) used by CarPlay.
 *
 * M4 step 1: serve GET /info with the CarPlay binary-plist descriptor. Pairing
 * (/pair-setup, /pair-verify) and /auth-setup are added next.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the RTSP listener on TCP port 7000 and a local self-test fetch. */
esp_err_t xc_airplay_start(void);

/** Build the /info binary plist into buf. Exposed for testing. */
esp_err_t xc_airplay_build_info(uint8_t *buf, size_t cap, size_t *out_len);

/** AirPlay device id (AP MAC) as a "AA:BB:.." string, used by iAP2 too. */
void xc_airplay_device_id(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
