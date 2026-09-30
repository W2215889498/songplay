/*
 * AirPlay screen (video) stream receiver.
 *
 * The phone connects to the TCP data port reported in the /stream SETUP reply.
 * Every message is a 128-byte AirPlayScreenHeader followed by a body:
 *   opcode 1 = clear VideoConfig (avcC/hvcC record),
 *   opcode 0 = ChaCha20-Poly1305 sealed H.264 frame, AAD = the 128-byte header,
 *              nonce = 8-byte little-endian frame counter, key = the DataStream
 *              output key from the SETUP.
 * Decrypted frames carry length-prefixed NALUs and are converted to Annex B.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the video listener; *out_port receives the bound TCP port. */
esp_err_t xc_video_start(const uint8_t key[32], uint16_t *out_port);

/** Stop the listener and drop the current connection. */
void xc_video_stop(void);

/** Number of decrypted H.264 frames received so far. */
uint32_t xc_video_frame_count(void);

/** True once the peer sent the codec configuration. */
bool xc_video_has_config(void);

/**
 * One-shot bring-up self test: encode a synthetic colour-bar frame with the
 * hardware H.264 encoder, decode it with the software decoder and draw it on
 * the panel. Used to validate the whole decode/display path at boot.
 */
void xc_video_lcd_selftest(void);

#ifdef __cplusplus
}
#endif
