#include "xc_bplist.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
    PB_STR,
    PB_INT,
    PB_BOOL,
    PB_DATA,
    PB_ARR,
    PB_DICT,
} pb_type_t;

typedef struct {
    pb_type_t type;
    const char *s;
    size_t slen;
    const uint8_t *data;
    size_t dlen;
    uint64_t u;
    int nrefs;
    int refs[XC_BPLIST_MAX_REF];
    int keyrefs[XC_BPLIST_MAX_REF];
} pb_obj_t;

struct xc_bplist {
    pb_obj_t obj[XC_BPLIST_MAX_OBJ];
    int n;
};

xc_bplist_t *xc_bplist_new(void)
{
    xc_bplist_t *b = calloc(1, sizeof(xc_bplist_t));
    return b;
}

void xc_bplist_free(xc_bplist_t *b)
{
    free(b);
}

static int add_obj(xc_bplist_t *b)
{
    if (b == NULL || b->n >= XC_BPLIST_MAX_OBJ) {
        return -1;
    }
    return b->n++;
}

int xc_bplist_str(xc_bplist_t *b, const char *s)
{
    if (b == NULL || s == NULL) {
        return -1;
    }
    size_t len = strlen(s);
    for (int i = 0; i < b->n; i++) {
        if (b->obj[i].type == PB_STR && b->obj[i].slen == len && memcmp(b->obj[i].s, s, len) == 0) {
            return i;
        }
    }
    int id = add_obj(b);
    if (id < 0) {
        return -1;
    }
    b->obj[id].type = PB_STR;
    b->obj[id].s = s;
    b->obj[id].slen = len;
    return id;
}

int xc_bplist_int(xc_bplist_t *b, uint64_t v)
{
    if (b == NULL) {
        return -1;
    }
    for (int i = 0; i < b->n; i++) {
        if (b->obj[i].type == PB_INT && b->obj[i].u == v) {
            return i;
        }
    }
    int id = add_obj(b);
    if (id < 0) {
        return -1;
    }
    b->obj[id].type = PB_INT;
    b->obj[id].u = v;
    return id;
}

int xc_bplist_bool(xc_bplist_t *b, bool v)
{
    if (b == NULL) {
        return -1;
    }
    for (int i = 0; i < b->n; i++) {
        if (b->obj[i].type == PB_BOOL && (b->obj[i].u != 0) == v) {
            return i;
        }
    }
    int id = add_obj(b);
    if (id < 0) {
        return -1;
    }
    b->obj[id].type = PB_BOOL;
    b->obj[id].u = v ? 1 : 0;
    return id;
}

int xc_bplist_data(xc_bplist_t *b, const void *p, size_t n)
{
    if (b == NULL || p == NULL) {
        return -1;
    }
    int id = add_obj(b);
    if (id < 0) {
        return -1;
    }
    b->obj[id].type = PB_DATA;
    b->obj[id].data = p;
    b->obj[id].dlen = n;
    return id;
}

int xc_bplist_array(xc_bplist_t *b, const int *refs, int n)
{
    if (b == NULL || refs == NULL || n < 0 || n > XC_BPLIST_MAX_REF) {
        return -1;
    }
    int id = add_obj(b);
    if (id < 0) {
        return -1;
    }
    b->obj[id].type = PB_ARR;
    b->obj[id].nrefs = n;
    memcpy(b->obj[id].refs, refs, sizeof(int) * n);
    return id;
}

int xc_bplist_dict(xc_bplist_t *b, const int *keys, const int *vals, int n)
{
    if (b == NULL || keys == NULL || vals == NULL || n < 0 || n > XC_BPLIST_MAX_REF) {
        return -1;
    }
    int id = add_obj(b);
    if (id < 0) {
        return -1;
    }
    b->obj[id].type = PB_DICT;
    b->obj[id].nrefs = n;
    memcpy(b->obj[id].keyrefs, keys, sizeof(int) * n);
    memcpy(b->obj[id].refs, vals, sizeof(int) * n);
    return id;
}

static int int_bytes(uint64_t v)
{
    if (v <= 0xff) {
        return 1;
    }
    if (v <= 0xffff) {
        return 2;
    }
    if (v <= 0xffffffffULL) {
        return 4;
    }
    if (v <= 0x7fffffffffffffffULL) {
        return 8;
    }
    /* Unsigned values above INT64_MAX (e.g. CarPlay streamConnectionIDs) use
     * the 16-byte form, matching CoreFoundation's encoder. */
    return 16;
}

static size_t int_obj_size(uint64_t v)
{
    return 1 + int_bytes(v);
}

/* Extra bytes of the extended-length suffix: the 0b1111 marker byte itself is
 * already counted by obj_size's leading +1, so only the integer object counts. */
static size_t marker_size(size_t count)
{
    return count < 15 ? 0 : int_obj_size(count);
}

static size_t obj_size(xc_bplist_t *b, int id, int refsize)
{
    pb_obj_t *o = &b->obj[id];
    switch (o->type) {
    case PB_STR:
        return 1 + marker_size(o->slen) + o->slen;
    case PB_DATA:
        return 1 + marker_size(o->dlen) + o->dlen;
    case PB_INT:
        return int_obj_size(o->u);
    case PB_BOOL:
        return 1;
    case PB_ARR:
        return 1 + marker_size((size_t)o->nrefs) + (size_t)o->nrefs * refsize;
    case PB_DICT:
        return 1 + marker_size((size_t)o->nrefs) + (size_t)o->nrefs * 2 * refsize;
    }
    return 0;
}

static void put_be(uint8_t **p, uint64_t v, int nbytes)
{
    for (int i = nbytes - 1; i >= 0; i--) {
        *(*p)++ = (uint8_t)(v >> (8 * i));
    }
}

static void write_int_obj(uint8_t **p, uint64_t v)
{
    int nb = int_bytes(v);
    int log = nb == 1 ? 0 : nb == 2 ? 1 : nb == 4 ? 2 : nb == 8 ? 3 : 4;
    *(*p)++ = (uint8_t)(0x10 | log);
    if (nb == 16) {
        memset(*p, 0, 8);
        *p += 8;
        put_be(p, v, 8);
    } else {
        put_be(p, v, nb);
    }
}

static void write_marker(uint8_t **p, uint8_t base, size_t count)
{
    if (count < 15) {
        *(*p)++ = (uint8_t)(base | count);
    } else {
        *(*p)++ = (uint8_t)(base | 0x0f);
        write_int_obj(p, count);
    }
}

static void write_ref(uint8_t **p, int ref, int refsize)
{
    if (refsize == 1) {
        *(*p)++ = (uint8_t)ref;
    } else {
        put_be(p, (uint64_t)ref, refsize);
    }
}

esp_err_t xc_bplist_serialize(xc_bplist_t *b, int root, uint8_t *out, size_t cap,
                              size_t *out_len)
{
    if (b == NULL || out == NULL || root < 0 || root >= b->n) {
        return ESP_ERR_INVALID_ARG;
    }

    int refsize = (b->n <= 0xff) ? 1 : 2;
    size_t offset = 8; /* after "bplist00" */
    size_t offsets[XC_BPLIST_MAX_OBJ];
    for (int i = 0; i < b->n; i++) {
        offsets[i] = offset;
        offset += obj_size(b, i, refsize);
    }
    size_t offset_table_offset = offset;
    /* The reference encoders use the smallest offset-table entry that fits. */
    int offset_int_size = offset_table_offset <= 0xff ? 1 : offset_table_offset <= 0xffff ? 2 : 4;
    size_t total = offset_table_offset + (size_t)b->n * offset_int_size + 32;
    if (total > cap) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *p = out;
    memcpy(p, "bplist00", 8);
    p += 8;

    for (int i = 0; i < b->n; i++) {
        pb_obj_t *o = &b->obj[i];
        switch (o->type) {
        case PB_STR:
            write_marker(&p, 0x50, o->slen);
            memcpy(p, o->s, o->slen);
            p += o->slen;
            break;
        case PB_DATA:
            write_marker(&p, 0x40, o->dlen);
            memcpy(p, o->data, o->dlen);
            p += o->dlen;
            break;
        case PB_INT:
            write_int_obj(&p, o->u);
            break;
        case PB_BOOL:
            *p++ = o->u ? 0x09 : 0x08;
            break;
        case PB_ARR:
            write_marker(&p, 0xa0, (size_t)o->nrefs);
            for (int k = 0; k < o->nrefs; k++) {
                write_ref(&p, o->refs[k], refsize);
            }
            break;
        case PB_DICT:
            write_marker(&p, 0xd0, (size_t)o->nrefs);
            for (int k = 0; k < o->nrefs; k++) {
                write_ref(&p, o->keyrefs[k], refsize);
            }
            for (int k = 0; k < o->nrefs; k++) {
                write_ref(&p, o->refs[k], refsize);
            }
            break;
        }
    }

    for (int i = 0; i < b->n; i++) {
        put_be(&p, offsets[i], offset_int_size);
    }

    /* Canonical 32-byte trailer: 5 unused bytes, sortVersion, then the sizes. */
    for (int i = 0; i < 5; i++) {
        *p++ = 0;
    }
    *p++ = 0; /* sortVersion */
    *p++ = (uint8_t)offset_int_size;
    *p++ = (uint8_t)refsize;
    put_be(&p, (uint64_t)b->n, 8);
    put_be(&p, (uint64_t)root, 8);
    put_be(&p, (uint64_t)offset_table_offset, 8);

    *out_len = (size_t)(p - out);
    return ESP_OK;
}
