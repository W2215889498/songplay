/*
 * TLV8 codec used by AirPlay pairing and iAP2 control messages.
 *
 * Each item is type(1) + length(1) + value(length). Values longer than 255
 * bytes are fragmented across consecutive items of the same type; readers must
 * re-concatenate fragments.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t type;
    const uint8_t *value;
    size_t len;
} xc_tlv8_item_t;

/** Append one TLV item (fragmenting values > 255) into buf at *pos. */
esp_err_t xc_tlv8_write(uint8_t *buf, size_t cap, size_t *pos,
                        uint8_t type, const uint8_t *value, size_t len);

/** Append a single byte TLV item. */
static inline esp_err_t xc_tlv8_write_u8(uint8_t *buf, size_t cap, size_t *pos,
                                         uint8_t type, uint8_t value)
{
    return xc_tlv8_write(buf, cap, pos, type, &value, 1);
}

/** Number of physical items in the buffer. */
size_t xc_tlv8_count(const uint8_t *buf, size_t len);

/** Read the nth physical item. Returns ESP_ERR_NOT_FOUND when out of range. */
esp_err_t xc_tlv8_get_nth(const uint8_t *buf, size_t len, size_t idx, xc_tlv8_item_t *out);

/**
 * Find the logical value for a type, concatenating fragments into out.
 * *out_len receives the total value length. Returns ESP_ERR_NOT_FOUND if absent.
 */
esp_err_t xc_tlv8_find(const uint8_t *buf, size_t len, uint8_t type,
                       uint8_t *out, size_t out_cap, size_t *out_len);

/** Convenience: find a single byte value. */
esp_err_t xc_tlv8_find_u8(const uint8_t *buf, size_t len, uint8_t type, uint8_t *out);

#ifdef __cplusplus
}
#endif
