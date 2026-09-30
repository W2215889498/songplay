/*
 * Minimal binary plist (bplist00) reader, used to parse AirPlay /stream SETUP
 * requests. Only the subset the protocol needs is supported: dict, array,
 * ASCII string, boolean, data and (un)signed integers.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t offset_table;
    int offset_int_size;
    int ref_size;
    int num_objects;
    int top_object;
} xc_plist_t;

/** Validate the trailer and locate the root object. */
esp_err_t xc_plist_open(const uint8_t *buf, size_t len, xc_plist_t *out);

/** Root object reference, or -1. */
int xc_plist_root(const xc_plist_t *p);

/** Look up a dict entry by key. Returns the value ref, or -1. */
int xc_plist_dict_get(const xc_plist_t *p, int dict_ref, const char *key);

/** Array helpers. */
int xc_plist_array_count(const xc_plist_t *p, int arr_ref);
int xc_plist_array_get(const xc_plist_t *p, int arr_ref, int index);

/** Read a value as a signed 64-bit integer (also accepts bool). */
bool xc_plist_get_int(const xc_plist_t *p, int ref, int64_t *out);

/**
 * Read an integer as an unsigned decimal string, matching the way iAP2 encodes
 * streamConnectionID (Apple allows negative signed values for large ids).
 */
bool xc_plist_get_u64_str(const xc_plist_t *p, int ref, char *out, size_t cap);

/** Read an ASCII string value. */
bool xc_plist_get_str(const xc_plist_t *p, int ref, char *out, size_t cap);

/** True when the object is a boolean true. */
bool xc_plist_get_bool(const xc_plist_t *p, int ref, bool *out);

#ifdef __cplusplus
}
#endif
