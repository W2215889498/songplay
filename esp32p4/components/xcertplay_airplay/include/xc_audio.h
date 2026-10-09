/*
 * AirPlay/CarPlay audio stream receiver (UDP RTP).
 *
 * Unlike the screen streams (TCP), each audio stream is a pair of UDP sockets:
 * the phone sends RTP datagrams to the data port reported in the /stream SETUP
 * reply and (optionally) RTCP to the control port. Wire layout per datagram
 * (xcertplay `AudioStream` / LIVI `livi_audio_stream`):
 *
 *   [12-byte RTP header][AES-ChaCha20-Poly1305 ciphertext][16-byte tag][8-byte LE nonce]
 *
 * AAD = header bytes 4..12 (timestamp + SSRC); the key is the DataStream output
 * key derived from the SETUP's streamConnectionID, like the screen streams.
 * The plaintext starts with the same 12-byte RTP header; the codec payload
 * follows it (16-bit big-endian PCM, one AAC access unit, or one Opus packet).
 *
 * Navigation prompts arrive as short-lived type-101 streams: the phone SETUPs
 * the stream when a prompt starts and TEARDOWNs it when it ends.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Open a UDP audio stream. \p type is the CarPlay stream type (100 main audio,
 * 101 alt audio, 102 main high audio/AAC), \p conn_id the phone's
 * streamConnectionID (echoed back in the reply), \p key the 32-byte DataStream
 * output key, \p format_bits the negotiated `audioFormat` value and
 * \p audio_type its `audioType` string ("default", "media", ...).
 * *out_data_port / *out_control_port receive the bound UDP ports.
 */
esp_err_t xc_audio_open(uint16_t type, uint64_t conn_id, const uint8_t key[32],
                        uint64_t format_bits, const char *audio_type,
                        uint16_t *out_data_port, uint16_t *out_control_port);

/** Stop the stream of the given type (TEARDOWN). Safe when not open. */
void xc_audio_close(uint16_t type);

/** Output volume in percent (0..100), applied to the speaker codec. */
void xc_audio_set_volume(int percent);

/** Boot self test: short beep through the speaker path. */
void xc_audio_tone_test(void);

#ifdef __cplusplus
}
#endif
