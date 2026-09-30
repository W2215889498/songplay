#include "xc_bplist_read.h"

#include <stdio.h>
#include <string.h>

static uint64_t be_to_u64(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

esp_err_t xc_plist_open(const uint8_t *buf, size_t len, xc_plist_t *out)
{
    if (buf == NULL || out == NULL || len < 8 + 32) {
        return ESP_ERR_INVALID_ARG;
    }
    if (memcmp(buf, "bplist00", 8) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint8_t *tr = buf + len - 32;
    out->buf = buf;
    out->len = len;
    out->offset_int_size = tr[6];
    out->ref_size = tr[7];
    out->num_objects = (int)be_to_u64(tr + 8, 8);
    out->top_object = (int)be_to_u64(tr + 16, 8);
    out->offset_table = (size_t)be_to_u64(tr + 24, 8);
    if (out->offset_int_size < 1 || out->offset_int_size > 8 || out->ref_size < 1 ||
        out->ref_size > 8 || out->num_objects <= 0 || out->top_object >= out->num_objects) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (out->offset_table + (size_t)out->num_objects * out->offset_int_size > len) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static const uint8_t *object_ptr(const xc_plist_t *p, int ref)
{
    if (ref < 0 || ref >= p->num_objects) {
        return NULL;
    }
    size_t off = (size_t)be_to_u64(p->buf + p->offset_table + (size_t)ref * p->offset_int_size,
                                   p->offset_int_size);
    if (off >= p->len) {
        return NULL;
    }
    return p->buf + off;
}

int xc_plist_root(const xc_plist_t *p)
{
    return p->top_object;
}

/* Reads a marker's count/size, consuming an inline integer when needed. */
static size_t read_count(const xc_plist_t *p, const uint8_t **cur, uint8_t low)
{
    if (low != 0x0f) {
        return low;
    }
    const uint8_t *c = *cur;
    if ((size_t)(c - p->buf) + 2 > p->len) {
        return 0;
    }
    uint8_t marker = *c++;
    if ((marker & 0xf0) != 0x10) {
        return 0;
    }
    int nbytes = 1 << (marker & 0x0f);
    if ((size_t)(c - p->buf) + nbytes > p->len) {
        return 0;
    }
    size_t v = (size_t)be_to_u64(c, nbytes);
    c += nbytes;
    *cur = c;
    return v;
}

static const uint8_t *value_ptr(const xc_plist_t *p, int ref, uint8_t *type, uint8_t *low,
                                size_t *count, size_t *data_off)
{
    const uint8_t *obj = object_ptr(p, ref);
    if (obj == NULL) {
        return NULL;
    }
    *type = *obj & 0xf0;
    *low = *obj & 0x0f;
    const uint8_t *cur = obj + 1;
    *count = read_count(p, &cur, *low);
    *data_off = (size_t)(cur - p->buf);
    return cur;
}

int xc_plist_dict_get(const xc_plist_t *p, int dict_ref, const char *key)
{
    uint8_t type, low;
    size_t count, off;
    if (value_ptr(p, dict_ref, &type, &low, &count, &off) == NULL || type != 0xd0) {
        return -1;
    }
    size_t entry = (size_t)p->ref_size;
    for (size_t i = 0; i < count; i++) {
        size_t key_at = off + i * entry;
        size_t val_at = off + (count + i) * entry;
        if (key_at + entry > p->len || val_at + entry > p->len) {
            return -1;
        }
        int key_ref = (int)be_to_u64(p->buf + key_at, p->ref_size);
        const uint8_t *ks = object_ptr(p, key_ref);
        if (ks == NULL) {
            continue;
        }
        if ((*ks & 0xf0) != 0x50) {
            continue;
        }
        size_t klen = (*ks & 0x0f);
        const uint8_t *kb = ks + 1;
        if (klen == 0x0f) {
            const uint8_t *c = kb;
            klen = read_count(p, &c, 0x0f);
            kb = c;
        }
        if (klen == strlen(key) && memcmp(kb, key, klen) == 0) {
            return (int)be_to_u64(p->buf + val_at, p->ref_size);
        }
    }
    return -1;
}

int xc_plist_array_count(const xc_plist_t *p, int arr_ref)
{
    uint8_t type, low;
    size_t count, off;
    if (value_ptr(p, arr_ref, &type, &low, &count, &off) == NULL || type != 0xa0) {
        return 0;
    }
    return (int)count;
}

int xc_plist_array_get(const xc_plist_t *p, int arr_ref, int index)
{
    uint8_t type, low;
    size_t count, off;
    if (value_ptr(p, arr_ref, &type, &low, &count, &off) == NULL || type != 0xa0) {
        return -1;
    }
    if (index < 0 || (size_t)index >= count) {
        return -1;
    }
    size_t at = off + (size_t)index * p->ref_size;
    if (at + p->ref_size > p->len) {
        return -1;
    }
    return (int)be_to_u64(p->buf + at, p->ref_size);
}

bool xc_plist_get_int(const xc_plist_t *p, int ref, int64_t *out)
{
    uint8_t type, low;
    size_t count, off;
    if (value_ptr(p, ref, &type, &low, &count, &off) == NULL) {
        return false;
    }
    if (type == 0x10) {
        int nbytes = 1 << low;
        if (off + (size_t)nbytes > p->len) {
            return false;
        }
        if (nbytes == 8) {
            *out = (int64_t)be_to_u64(p->buf + off, 8);
        } else {
            uint64_t v = be_to_u64(p->buf + off, nbytes);
            *out = (int64_t)v;
        }
        return true;
    }
    if (type == 0x00) {
        *out = (low == 0x09) ? 1 : 0;
        return true;
    }
    return false;
}

bool xc_plist_get_u64_str(const xc_plist_t *p, int ref, char *out, size_t cap)
{
    int64_t v = 0;
    if (!xc_plist_get_int(p, ref, &v)) {
        return false;
    }
    if (v < 0) {
        /* Apple stores large stream ids as negative signed values. */
        snprintf(out, cap, "%llu", (unsigned long long)(uint64_t)v);
    } else {
        snprintf(out, cap, "%lld", (long long)v);
    }
    return true;
}

bool xc_plist_get_str(const xc_plist_t *p, int ref, char *out, size_t cap)
{
    uint8_t type, low;
    size_t count, off;
    const uint8_t *data = value_ptr(p, ref, &type, &low, &count, &off);
    if (data == NULL || type != 0x50 || cap == 0) {
        return false;
    }
    size_t n = count < cap - 1 ? count : cap - 1;
    memcpy(out, p->buf + off, n);
    out[n] = '\0';
    return true;
}

bool xc_plist_get_bool(const xc_plist_t *p, int ref, bool *out)
{
    const uint8_t *obj = object_ptr(p, ref);
    if (obj == NULL) {
        return false;
    }
    if ((*obj & 0xf0) != 0x00) {
        return false;
    }
    *out = ((*obj & 0x0f) == 0x09);
    return true;
}
