/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **PMTiles, version 3, read** (`docs/maps.md` M2) - the format a region of
 * the map is kept in on the disk: one file, a header, a directory of runs
 * of tiles, and the tiles. Protomaps' specification, followed as written,
 * so an extract made by anyone's tool opens here; `tools/mapcity.py` writes
 * the made-up Port Alder in it for the tests.
 *
 * **A tile is found by one number**, its place along a Hilbert curve after
 * every tile of the zooms before - so nearby tiles have nearby numbers, sit
 * near each other in the file, and a directory of runs stays short. The
 * directory is sorted by that number and found by halving.
 *
 * **Directories may be gzipped**, as every real archive's are; the root is
 * inflated once when the archive opens and kept, a leaf when a tile under
 * it is first asked for, and the last one kept. Tiles are handed back as
 * they are stored - the Map Kit inflates one when it decodes it.
 */

#include "pmtiles.h"

#include <stdlib.h>
#include <string.h>

#include "kits/compress/gzip.h"

const char *pmt_said(int result)
{
    switch (result) {
    case PMT_OK:          return "found";
    case PMT_NOT_FOUND:   return "no such tile";
    case PMT_NOT_PMTILES: return "not a PMTiles archive of version 3";
    case PMT_DAMAGED:     return "the archive is damaged: something points past its end";
    case PMT_UNSUPPORTED: return "compressed in a way Kosmos does not read (only none and gzip)";
    case PMT_NO_MEMORY:   return "no memory for its directory";
    }

    return "unknown";
}

static uint64_t u64(const uint8_t *p)
{
    uint64_t v = 0;

    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];

    return v;
}

static int32_t i32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16
                     | (uint32_t)p[3] << 24);
}

uint64_t pmt_tile_id(unsigned z, uint32_t x, uint32_t y)
{
    uint64_t acc = (((uint64_t)1 << (2 * z)) - 1) / 3;
    uint64_t d = 0;
    uint64_t s;

    for (s = z ? ((uint64_t)1 << z) / 2 : 0; s > 0; s /= 2) {
        uint64_t rx = (x & s) ? 1 : 0, ry = (y & s) ? 1 : 0;

        d += s * s * ((3 * rx) ^ ry);

        if (ry == 0) {
            uint32_t t;

            if (rx == 1) {
                x = (uint32_t)(s - 1 - x);
                y = (uint32_t)(s - 1 - y);
            }

            t = x, x = y, y = t;
        }
    }

    return acc + d;
}

/* A varint at `*p`, before `end`; false when it runs off. */
static int varint(const uint8_t **p, const uint8_t *end, uint64_t *out)
{
    uint64_t v = 0;
    int shift = 0;

    while (*p < end && shift < 64) {
        uint8_t b = *(*p)++;

        v |= (uint64_t)(b & 0x7f) << shift;

        if (!(b & 0x80)) {
            *out = v;
            return 1;
        }

        shift += 7;
    }

    return 0;
}

/* What `kosmos_gunzip` puts, gathered into a growing buffer. */
struct gather { uint8_t *bytes; size_t n, cap; int short_of_memory; };

static int gather_put(void *user, const uint8_t *bytes, size_t n)
{
    struct gather *g = user;

    if (g->n + n > g->cap) {
        size_t cap = g->cap ? g->cap : 4096;
        uint8_t *grown;

        while (cap < g->n + n) cap *= 2;

        if ((grown = realloc(g->bytes, cap)) == NULL) {
            g->short_of_memory = 1;
            return 0;
        }

        g->bytes = grown, g->cap = cap;
    }

    memcpy(g->bytes + g->n, bytes, n);
    g->n += n;
    return 1;
}

/* A directory at `off`, `len` bytes, decoded into `*out`. */
static int directory(struct pmtiles *a, uint64_t off, uint64_t len,
                     struct pmt_entry **out, size_t *count)
{
    const uint8_t *p, *end;
    struct gather g = { 0 };
    struct pmt_entry *e;
    uint64_t n, v, last = 0;
    int result = PMT_DAMAGED;

    if (off > a->len || len > a->len - off) return PMT_DAMAGED;

    p = a->data + off, end = p + len;

    if (a->internal_gzip) {
        size_t got;

        if (kosmos_gunzip(p, (size_t)len, a->gunzip, gather_put, &g, &got) != GUNZIP_WHOLE) {
            free(g.bytes);
            return g.short_of_memory ? PMT_NO_MEMORY : PMT_DAMAGED;
        }

        p = g.bytes, end = g.bytes + g.n;
    }

    if (!varint(&p, end, &n) || n > (uint64_t)(end - p)) goto out;

    if ((e = calloc(n ? (size_t)n : 1, sizeof *e)) == NULL) {
        result = PMT_NO_MEMORY;
        goto out;
    }

    for (uint64_t i = 0; i < n; i++) {
        if (!varint(&p, end, &v)) goto bad;
        last += v;
        e[i].id = last;
    }

    for (uint64_t i = 0; i < n; i++) {
        if (!varint(&p, end, &v)) goto bad;
        e[i].run = (uint32_t)v;
    }

    for (uint64_t i = 0; i < n; i++) {
        if (!varint(&p, end, &v)) goto bad;
        e[i].length = (uint32_t)v;
    }

    for (uint64_t i = 0; i < n; i++) {
        if (!varint(&p, end, &v)) goto bad;

        /* 0: straight after the one before; otherwise the offset plus one. */
        if (v == 0 && i > 0) {
            e[i].offset = e[i - 1].offset + e[i - 1].length;
        } else if (v == 0) {
            goto bad;
        } else {
            e[i].offset = v - 1;
        }
    }

    *out = e, *count = (size_t)n;
    result = PMT_OK;
    goto out;

bad:
    free(e);
out:
    free(g.bytes);
    return result;
}

int pmt_open(struct pmtiles *a, const uint8_t *data, size_t len, void *gunzip)
{
    memset(a, 0, sizeof *a);
    a->data = data, a->len = len, a->gunzip = gunzip;
    a->leaf_at = ~(uint64_t)0;

    if (len < 127 || memcmp(data, "PMTiles", 7) != 0 || data[7] != 3) {
        return PMT_NOT_PMTILES;
    }

    a->root_off = u64(data + 8),  a->root_len = u64(data + 16);
    a->meta_off = u64(data + 24), a->meta_len = u64(data + 32);
    a->leaf_off = u64(data + 40), a->leaf_len = u64(data + 48);
    a->data_off = u64(data + 56), a->data_len = u64(data + 64);

    /* 97: the directories' compression, 98: the tiles', 99: the tiles' kind.
     * 1 none, 2 gzip; 0 "unknown" read as none, as the specification's own
     * reader does. */
    if (data[97] > 2 || data[98] > 2) return PMT_UNSUPPORTED;

    a->internal_gzip = data[97] == 2;
    a->tile_gzip = data[98] == 2;
    a->tile_type = data[99];
    a->min_zoom = data[100], a->max_zoom = data[101];
    a->min_lon_e7 = i32(data + 102), a->min_lat_e7 = i32(data + 106);
    a->max_lon_e7 = i32(data + 110), a->max_lat_e7 = i32(data + 114);
    a->centre_zoom = data[118];
    a->centre_lon_e7 = i32(data + 119), a->centre_lat_e7 = i32(data + 123);

    if (a->data_off > len || a->data_len > len - a->data_off) return PMT_DAMAGED;

    return directory(a, a->root_off, a->root_len, &a->root, &a->nroot);
}

void pmt_close(struct pmtiles *a)
{
    free(a->root);
    free(a->leaf);
    a->root = a->leaf = NULL;
    a->nroot = a->nleaf = 0;
}

/* The entry whose run holds `id`, or NULL - the last at or below it. */
static const struct pmt_entry *lookup(const struct pmt_entry *e, size_t n, uint64_t id)
{
    size_t lo = 0, hi = n;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (e[mid].id <= id) lo = mid + 1; else hi = mid;
    }

    if (lo == 0) return NULL;

    e = &e[lo - 1];

    /* A leaf's entry (run 0) covers everything up to the next entry. */
    if (e->run == 0 || id < e->id + e->run) return e;

    return NULL;
}

int pmt_find(struct pmtiles *a, unsigned z, uint32_t x, uint32_t y,
             const uint8_t **bytes, size_t *n)
{
    uint64_t id;
    const struct pmt_entry *dir = a->root;
    size_t count = a->nroot;

    if (z > 31 || z < a->min_zoom || z > a->max_zoom) return PMT_NOT_FOUND;
    if (x >= (1u << z) || y >= (1u << z)) return PMT_NOT_FOUND;

    id = pmt_tile_id(z, x, y);

    /* At most four levels deep, as the specification promises. */
    for (int depth = 0; depth < 4; depth++) {
        const struct pmt_entry *e = lookup(dir, count, id);

        if (e == NULL) return PMT_NOT_FOUND;

        if (e->run > 0) {
            if (a->data_off + e->offset > a->len || e->length > a->len - a->data_off - e->offset) {
                return PMT_DAMAGED;
            }

            *bytes = a->data + a->data_off + e->offset;
            *n = e->length;
            return PMT_OK;
        }

        /* A leaf directory: decoded, unless it is the one kept. */
        if (a->leaf_at != e->offset) {
            uint64_t at = e->offset, len = e->length;
            int r;

            free(a->leaf);
            a->leaf = NULL, a->nleaf = 0, a->leaf_at = ~(uint64_t)0;

            if ((r = directory(a, a->leaf_off + at, len, &a->leaf, &a->nleaf)) != PMT_OK) {
                return r;
            }

            a->leaf_at = at;
        }

        dir = a->leaf, count = a->nleaf;
    }

    return PMT_NOT_FOUND;
}
