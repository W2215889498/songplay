/* Minimal C wrapper around the openh264 (welsdec) software decoder.
 *
 * The ESP32-P4 has no H.264 hardware decoder and the bundled tinyh264 SW
 * decoder is constrained-baseline only, while the iPhone's CarPlay stream is
 * High profile + CABAC. openh264's decoder handles both.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wels_dec wels_dec_t;

wels_dec_t *wels_dec_create(void);
void wels_dec_destroy(wels_dec_t *d);

/* Feed one Annex-B access unit (SPS/PPS included as needed). Returns 1 when a
 * picture is ready, 0 otherwise, -1 on fatal error. */
int wels_dec_decode(wels_dec_t *d, const uint8_t *data, size_t len);

int wels_dec_width(const wels_dec_t *d);
int wels_dec_height(const wels_dec_t *d);
int wels_dec_stride(const wels_dec_t *d, int plane);          /* 0 = Y, 1/2 = U/V */
const uint8_t *wels_dec_plane(const wels_dec_t *d, int plane); /* valid until next decode */

#ifdef __cplusplus
}
#endif
