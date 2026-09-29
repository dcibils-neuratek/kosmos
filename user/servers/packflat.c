/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A flat table in `sys.pack`'s format (`packflat.h`). The format is
 * `lua/kosmos/serialize.c`'s: a type byte and its payload - an integer or a
 * float's bits in eight bytes, a string as a four-byte length and its bytes,
 * little-endian - and a table as its own byte, its keys and values in
 * turn, and an end byte.
 */

#include "packflat.h"

#include <stdio.h>
#include <string.h>

#define T_TABLE 6u
#define T_END   7u

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;

    for (int i = 7; i >= 0; i--) {
        v = v << 8 | p[i];
    }

    return v;
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16
           | (uint32_t)p[3] << 24;
}

/* One scalar at `*at`, moved past. */
static int read_value(const uint8_t *b, size_t len, size_t *at,
                      struct packflat_value *out)
{
    uint8_t type;

    if (*at >= len) {
        return PACKFLAT_E_MALFORMED;
    }

    type = b[(*at)++];
    memset(out, 0, sizeof *out);
    out->type = type;

    switch (type) {
    case PACKFLAT_FALSE:
    case PACKFLAT_TRUE:
        return PACKFLAT_OK;

    case PACKFLAT_INT:
    case PACKFLAT_FLOAT:
        if (len - *at < 8) {
            return PACKFLAT_E_MALFORMED;
        }

        out->bits = get_u64(b + *at);
        *at += 8;
        return PACKFLAT_OK;

    case PACKFLAT_STRING:
        if (len - *at < 4) {
            return PACKFLAT_E_MALFORMED;
        }

        out->len = get_u32(b + *at);
        *at += 4;

        if (len - *at < out->len) {
            return PACKFLAT_E_MALFORMED;
        }

        out->text = (const char *)b + *at;
        *at += out->len;
        return PACKFLAT_OK;

    case T_TABLE:
    case 0:                             /* nil */
        return PACKFLAT_E_NOT_FLAT;

    default:
        return PACKFLAT_E_MALFORMED;
    }
}

int packflat_read(const void *buf, size_t len, struct packflat *out)
{
    const uint8_t *b = buf;
    size_t at = 0;

    out->count = 0;

    if (len == 0 || b[0] != T_TABLE) {
        return PACKFLAT_E_MALFORMED;
    }

    at = 1;

    for (;;) {
        int r;

        if (at >= len) {
            return PACKFLAT_E_MALFORMED;        /* no end */
        }

        if (b[at] == T_END) {
            at++;
            break;
        }

        if (out->count == PACKFLAT_MAX) {
            return PACKFLAT_E_TOO_MANY;
        }

        r = read_value(b, len, &at, &out->key[out->count]);

        if (r == PACKFLAT_OK) {
            r = read_value(b, len, &at, &out->value[out->count]);
        }

        if (r != PACKFLAT_OK) {
            return r;
        }

        out->count++;
    }

    return at == len ? PACKFLAT_OK : PACKFLAT_E_MALFORMED;
}

static bool put(uint8_t *b, size_t cap, size_t *at, const void *bytes, size_t n)
{
    if (cap - *at < n) {
        return false;
    }

    memcpy(b + *at, bytes, n);
    *at += n;
    return true;
}

static bool put_value(uint8_t *b, size_t cap, size_t *at,
                      const struct packflat_value *v)
{
    uint8_t word[8];

    if (!put(b, cap, at, &v->type, 1)) {
        return false;
    }

    switch (v->type) {
    case PACKFLAT_INT:
    case PACKFLAT_FLOAT:
        for (int i = 0; i < 8; i++) {
            word[i] = (uint8_t)(v->bits >> (8 * i));
        }

        return put(b, cap, at, word, 8);

    case PACKFLAT_STRING:
        for (int i = 0; i < 4; i++) {
            word[i] = (uint8_t)(v->len >> (8 * i));
        }

        return put(b, cap, at, word, 4) && put(b, cap, at, v->text, v->len);

    default:
        return true;
    }
}

int packflat_write(const struct packflat *t, void *buf, size_t cap, size_t *len)
{
    uint8_t *b = buf;
    uint8_t mark = T_TABLE;
    size_t at = 0;

    if (!put(b, cap, &at, &mark, 1)) {
        return PACKFLAT_E_TOO_BIG;
    }

    for (uint32_t i = 0; i < t->count; i++) {
        if (!put_value(b, cap, &at, &t->key[i])
            || !put_value(b, cap, &at, &t->value[i])) {
            return PACKFLAT_E_TOO_BIG;
        }
    }

    mark = T_END;

    if (!put(b, cap, &at, &mark, 1)) {
        return PACKFLAT_E_TOO_BIG;
    }

    *len = at;
    return PACKFLAT_OK;
}

static int find(const struct packflat *t, const char *name, size_t len)
{
    for (uint32_t i = 0; i < t->count; i++) {
        if (t->key[i].type == PACKFLAT_STRING && t->key[i].len == len
            && memcmp(t->key[i].text, name, len) == 0) {
            return (int)i;
        }
    }

    return -1;
}

const struct packflat_value *packflat_get(const struct packflat *t,
                                          const char *name, size_t len)
{
    int i = find(t, name, len);

    return i < 0 ? NULL : &t->value[i];
}

int packflat_set(struct packflat *t, const char *name, size_t len,
                 const struct packflat_value *value)
{
    int i = find(t, name, len);

    if (value == NULL) {
        if (i >= 0) {
            t->count--;
            t->key[i] = t->key[t->count];
            t->value[i] = t->value[t->count];
        }

        return PACKFLAT_OK;
    }

    if (i < 0) {
        if (t->count == PACKFLAT_MAX) {
            return PACKFLAT_E_TOO_MANY;
        }

        i = (int)t->count++;
        t->key[i] = packflat_string(name, len);
    }

    t->value[i] = *value;
    return PACKFLAT_OK;
}

size_t packflat_text(const struct packflat_value *v, char *out, size_t cap)
{
    int n = 0;

    switch (v->type) {
    case PACKFLAT_FALSE:
        n = snprintf(out, cap, "false");
        break;

    case PACKFLAT_TRUE:
        n = snprintf(out, cap, "true");
        break;

    case PACKFLAT_INT:
        n = snprintf(out, cap, "%lld", (long long)(int64_t)v->bits);
        break;

    case PACKFLAT_FLOAT: {
        double d;

        memcpy(&d, &v->bits, sizeof d);
        n = snprintf(out, cap, "%.14g", d);

        /* Lua's own rule: a float that prints as an integer says it is not. */
        if (n > 0 && (size_t)n + 2 < cap && out[strspn(out, "-0123456789")] == '\0') {
            out[n++] = '.';
            out[n++] = '0';
            out[n] = '\0';
        }

        break;
    }

    case PACKFLAT_STRING:
        n = (int)(v->len < cap ? v->len : cap);
        memcpy(out, v->text, (size_t)n);
        break;

    default:
        break;
    }

    return n < 0 ? 0 : (size_t)n;
}

struct packflat_value packflat_int(int64_t n)
{
    struct packflat_value v;

    memset(&v, 0, sizeof v);
    v.type = PACKFLAT_INT;
    v.bits = (uint64_t)n;
    return v;
}

struct packflat_value packflat_string(const char *text, size_t len)
{
    struct packflat_value v;

    memset(&v, 0, sizeof v);
    v.type = PACKFLAT_STRING;
    v.text = text;
    v.len = (uint32_t)len;
    return v;
}
