/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A flattened device tree, written - `fdt.h` has the shape.
 *
 * The structure block is written straight into the buffer after a 40-byte
 * header and the 16-byte empty reservation map; the names are gathered
 * beside it and copied after it at the end, when the header's offsets are
 * known.
 */

#include <string.h>

#include "fdt.h"

#define FDT_MAGIC       0xd00dfeedu
#define FDT_BEGIN_NODE  1u
#define FDT_END_NODE    2u
#define FDT_PROP        3u
#define FDT_END         9u

#define HEADER          40u
#define RESERVE_MAP     16u                 /* one terminating entry */
#define STRUCT_AT       (HEADER + RESERVE_MAP)

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Bytes onto the structure block, padded to a word with zeros. */
static void emit(struct fdt *f, const void *data, size_t len)
{
    size_t padded = (len + 3u) & ~(size_t)3u;

    if (f->full || STRUCT_AT + f->structure + padded > f->cap) {
        f->full = 1;
        return;
    }

    memcpy(f->buf + STRUCT_AT + f->structure, data, len);
    memset(f->buf + STRUCT_AT + f->structure + len, 0, padded - len);
    f->structure += padded;
}

static void emit32(struct fdt *f, uint32_t v)
{
    uint8_t b[4];

    put32(b, v);
    emit(f, b, 4);
}

/* A property name's offset in the strings block, added once. */
static uint32_t name_at(struct fdt *f, const char *name)
{
    size_t n = strlen(name) + 1, at = 0;

    while (at < f->names_len) {
        if (strcmp(f->names + at, name) == 0) {
            return (uint32_t)at;
        }

        at += strlen(f->names + at) + 1;
    }

    if (f->names_len + n > sizeof(f->names)) {
        f->full = 1;
        return 0;
    }

    memcpy(f->names + f->names_len, name, n);
    f->names_len += n;
    return (uint32_t)(f->names_len - n);
}

void fdt_begin(struct fdt *f, void *buf, size_t cap)
{
    memset(f, 0, sizeof(*f));
    f->buf = buf;
    f->cap = cap;

    if (cap < STRUCT_AT) {
        f->full = 1;
        return;
    }

    memset(f->buf, 0, STRUCT_AT);
}

void fdt_node(struct fdt *f, const char *name)
{
    emit32(f, FDT_BEGIN_NODE);
    emit(f, name, strlen(name) + 1);
}

void fdt_end_node(struct fdt *f)
{
    emit32(f, FDT_END_NODE);
}

void fdt_prop(struct fdt *f, const char *name, const void *data, size_t len)
{
    emit32(f, FDT_PROP);
    emit32(f, (uint32_t)len);
    emit32(f, name_at(f, name));
    emit(f, data, len);
}

void fdt_prop_str(struct fdt *f, const char *name, const char *value)
{
    fdt_prop(f, name, value, strlen(value) + 1);
}

void fdt_prop_u32(struct fdt *f, const char *name, uint32_t value)
{
    fdt_prop_cells(f, name, &value, 1);
}

void fdt_prop_cells(struct fdt *f, const char *name, const uint32_t *cells, size_t n)
{
    uint8_t b[4 * 32];
    size_t i;

    if (n > 32) {
        f->full = 1;
        return;
    }

    for (i = 0; i < n; i++) {
        put32(b + 4 * i, cells[i]);
    }

    fdt_prop(f, name, b, 4 * n);
}

void fdt_prop_empty(struct fdt *f, const char *name)
{
    fdt_prop(f, name, "", 0);
}

size_t fdt_finish(struct fdt *f)
{
    size_t strings_at, total;

    emit32(f, FDT_END);
    strings_at = STRUCT_AT + f->structure;
    total = strings_at + f->names_len;

    if (f->full || total > f->cap) {
        return 0;
    }

    memcpy(f->buf + strings_at, f->names, f->names_len);

    put32(f->buf + 0, FDT_MAGIC);
    put32(f->buf + 4, (uint32_t)total);
    put32(f->buf + 8, STRUCT_AT);               /* off_dt_struct */
    put32(f->buf + 12, (uint32_t)strings_at);   /* off_dt_strings */
    put32(f->buf + 16, HEADER);                 /* off_mem_rsvmap */
    put32(f->buf + 20, 17);                     /* version */
    put32(f->buf + 24, 16);                     /* last_comp_version */
    put32(f->buf + 28, 0);                      /* boot_cpuid_phys */
    put32(f->buf + 32, (uint32_t)f->names_len); /* size_dt_strings */
    put32(f->buf + 36, (uint32_t)f->structure); /* size_dt_struct */
    return total;
}
