/*
 * Bonjour advertising for the AirPlay/CarPlay accessory.
 *
 * The iPhone finds the head unit through the _airplay._tcp service. The TXT
 * records must match what xcertplay advertises, otherwise the phone ignores the
 * receiver.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_AIRPLAY_PORT 7000
#define XC_AIRPLAY_SRCVERS "950.7.1"

/** Start mDNS and advertise _airplay._tcp (and the CarPlay control service). */
esp_err_t xc_mdns_start(void);

#ifdef __cplusplus
}
#endif
