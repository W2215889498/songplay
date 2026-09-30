/*
 * Minimal binary plist (bplist00) writer used by the AirPlay /info response.
 *
 * Objects are referenced by integer ids; strings and small ints are interned so
 * the object table stays small (bplist dict keys are themselves objects).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_BPLIST_MAX_OBJ 512
#define XC_BPLIST_MAX_REF 48

typedef struct xc_bplist xc_bplist_t;

xc_bplist_t *xc_bplist_new(void);
void xc_bplist_free(xc_bplist_t *b);

int xc_bplist_str(xc_bplist_t *b, const char *s);
int xc_bplist_int(xc_bplist_t *b, uint64_t v);
int xc_bplist_bool(xc_bplist_t *b, bool v);
int xc_bplist_data(xc_bplist_t *b, const void *p, size_t n);
int xc_bplist_array(xc_bplist_t *b, const int *refs, int n);
int xc_bplist_dict(xc_bplist_t *b, const int *keys, const int *vals, int n);

esp_err_t xc_bplist_serialize(xc_bplist_t *b, int root, uint8_t *out, size_t cap,
                              size_t *out_len);

#ifdef __cplusplus
}
#endif
