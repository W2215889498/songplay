/*
 * Networking bring-up for the CarPlay head unit.
 *
 * The ESP32-P4 has no radio of its own; Wi-Fi comes from the onboard ESP32-C6
 * through ESP-Hosted + esp_wifi_remote, so this is plain esp_wifi API.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_AP_SSID "xcertplay-CarPlay"
#define XC_AP_PASSWORD "xcertplay123"
#define XC_AP_CHANNEL 6
#define XC_AP_MAX_STA 4

/** Start the Wi-Fi softAP (used by the iPhone for the CarPlay link). */
esp_err_t xc_net_start_ap(void);

/** True once the softAP has started successfully. */
bool xc_net_ap_ready(void);

#ifdef __cplusplus
}
#endif
