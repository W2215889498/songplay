#include "xc_tlv8.h"

#include <string.h>

esp_err_t xc_tlv8_write(uint8_t *buf, size_t cap, size_t *pos,
                        uint8_t type, const uint8_t *value, size_t len)
{
    size_t p = *pos;

    if (len == 0) {
        if (p + 2 > cap) {
            return ESP_ERR_INVALID_SIZE;
        }
        buf[p++] = type;
        buf[p++] = 0;
        *pos = p;
        return ESP_OK;
    }

    size_t remaining = len;
    const uint8_t *cur = value;
    while (remaining > 0) {
        size_t chunk = remaining > 255 ? 255 : remaining;
        if (p + 2 + chunk > cap) {
            return ESP_ERR_INVALID_SIZE;
        }
        buf[p++] = type;
        buf[p++] = (uint8_t)chunk;
        memcpy(buf + p, cur, chunk);
        p += chunk;
        cur += chunk;
        remaining -= chunk;
    }
    *pos = p;
    return ESP_OK;
}

size_t xc_tlv8_count(const uint8_t *buf, size_t len)
{
    size_t count = 0;
    size_t p = 0;
    while (p + 2 <= len) {
        size_t ilen = buf[p + 1];
        if (p + 2 + ilen > len) {
            break;
        }
        count++;
        p += 2 + ilen;
    }
    return count;
}

esp_err_t xc_tlv8_get_nth(const uint8_t *buf, size_t len, size_t idx, xc_tlv8_item_t *out)
{
    size_t p = 0;
    size_t count = 0;
    while (p + 2 <= len) {
        size_t ilen = buf[p + 1];
        if (p + 2 + ilen > len) {
            break;
        }
        if (count == idx) {
            out->type = buf[p];
            out->value = buf + p + 2;
            out->len = ilen;
            return ESP_OK;
        }
        count++;
        p += 2 + ilen;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t xc_tlv8_find(const uint8_t *buf, size_t len, uint8_t type,
                       uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t p = 0;
    size_t total = 0;
    bool found = false;

    while (p + 2 <= len) {
        size_t ilen = buf[p + 1];
        if (p + 2 + ilen > len) {
            break;
        }
        if (buf[p] == type) {
            if (total + ilen > out_cap) {
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(out + total, buf + p + 2, ilen);
            total += ilen;
            found = true;
        }
        p += 2 + ilen;
    }

    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }
    if (out_len) {
        *out_len = total;
    }
    return ESP_OK;
}

esp_err_t xc_tlv8_find_u8(const uint8_t *buf, size_t len, uint8_t type, uint8_t *out)
{
    uint8_t tmp[2];
    size_t total = 0;
    esp_err_t err = xc_tlv8_find(buf, len, type, tmp, sizeof(tmp), &total);
    if (err != ESP_OK) {
        return err;
    }
    if (total != 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    *out = tmp[0];
    return ESP_OK;
}
