/*
 * Caller-ID overlay card for the instrument-cluster stream.
 *
 * The cluster area shows the phone's H.264 navigation map. This module renders
 * a small dark card (LVGL canvas, CJK font) and blends it INTO the video
 * frame buffer before the frame is pushed to the panel. That way every panel
 * write carries the card and the map in one go: no transient "card-less"
 * frame, no flicker.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create the canvas and timers; needs the display/LVGL to be up. */
esp_err_t xc_callcard_init(void);

/** CallStateUpdate receiver (see xc_iap2.h for field meanings). */
void xc_callcard_on_call(int status, int direction, int service, const char *name,
                         const char *number);

/**
 * Blend the card over the freshly converted RGB565 frame (in place), before
 * the frame is drawn to the panel. No-op when no call is showing.
 */
void xc_callcard_compose(uint16_t *video, int cw, int ch);

#ifdef __cplusplus
}
#endif
