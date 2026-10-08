/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A Mapbox Vector Tile, decoded** (`docs/maps.md` M2) - the format every
 * vector map is served in: a protobuf of layers, each a list of features
 * whose properties are pairs of indices into the layer's own tables of keys
 * and values, and whose geometry is a stream of commands - move to, line
 * to, close the ring - with each point a zigzagged step from the last.
 * Specification 2.1, followed as written.
 *
 * **Decoded once, for every frame after.** What a map draws a hundred times
 * a second is kept as plain arrays - features, their parts, their points as
 * floats in the tile's units - so drawing walks memory and never protobuf.
 * Only the properties a map uses are kept: `class`, `subclass`, `name`,
 * `rank` and `brunnel`, as strings copied into the tile's own pool, so the
 * bytes it came from can go.
 *
 * **Two passes over a layer**, because the specification lets a layer's
 * keys and values come after the features that point into them: the first
 * finds the tables, the second reads the features.
 */

#include "mvt.h"

#include <stdlib.h>
#include <string.h>

static const struct { const char *name; int layer; } LAYERS[] = {
    { "water", L_WATER }, { "waterway", L_WATERWAY }, { "landcover", L_LANDCOVER },
    { "landuse", L_LANDUSE }, { "park", L_PARK }, { "building", L_BUILDING },
    { "transportation", L_TRANSPORTATION }, { "transportation_name", L_TRANSPORTATION_NAME },
    { "place", L_PLACE }, { "water_name", L_WATER_NAME }, { "poi", L_POI },
    { "boundary", L_BOUNDARY }, { "aeroway", L_AEROWAY },
};

int mvt_layer_of(const char *name, size_t len)
{
    for (size_t i = 0; i < sizeof LAYERS / sizeof LAYERS[0]; i++) {
        if (strlen(LAYERS[i].name) == len && memcmp(LAYERS[i].name, name, len) == 0) {
            return LAYERS[i].layer;
        }
    }

    return L_OTHER;
}

/* ---- protobuf, as much as a vector tile uses ---- */

struct pb { const uint8_t *p, *end; };

static int pb_varint(struct pb *b, uint64_t *out)
{
    uint64_t v = 0;

    for (int shift = 0; b->p < b->end && shift < 64; shift += 7) {
        uint8_t c = *b->p++;

        v |= (uint64_t)(c & 0x7f) << shift;

        if (!(c & 0x80)) {
            *out = v;
            return 1;
        }
    }

    return 0;
}

/* The next field: its number and wire type, and for a length-delimited one
 * its bytes in `*sub`. Others are skipped over. 0 at the end or when cut. */
static int pb_next(struct pb *b, uint32_t *field, uint32_t *wire, uint64_t *v, struct pb *sub)
{
    uint64_t k, len;

    if (b->p >= b->end || !pb_varint(b, &k)) return 0;

    *field = (uint32_t)(k >> 3), *wire = (uint32_t)(k & 7);

    switch (*wire) {
    case 0:
        return pb_varint(b, v);
    case 1:
        if (b->end - b->p < 8) return 0;
        memcpy(v, b->p, 8);
        b->p += 8;
        return 1;
    case 5: {
        uint32_t w;

        if (b->end - b->p < 4) return 0;
        memcpy(&w, b->p, 4);
        *v = w;
        b->p += 4;
        return 1;
    }
    case 2:
        if (!pb_varint(b, &len) || len > (uint64_t)(b->end - b->p)) return 0;
        sub->p = b->p, sub->end = b->p + len;
        b->p += len;
        return 1;
    }

    return 0;
}

/* ---- the tile's own arrays, grown as they fill ---- */

static int grow(void **p, size_t *cap, size_t need, size_t size)
{
    size_t c = *cap ? *cap : 64;
    void *n;

    if (need <= *cap) return 1;

    while (c < need) c *= 2;

    if ((n = realloc(*p, c * size)) == NULL) return 0;

    *p = n, *cap = c;
    return 1;
}

/* A string kept in the pool, by its offset - the pool may move as it
 * grows, so offsets are turned into pointers once decoding is done. */
static long keep(struct mvt_tile *t, const uint8_t *s, size_t n)
{
    long at = (long)t->nstrings;

    if (!grow((void **)&t->strings, &t->cap_s, t->nstrings + n + 1, 1)) return -1;

    memcpy(t->strings + t->nstrings, s, n);
    t->strings[t->nstrings + n] = '\0';
    t->nstrings += n + 1;
    return at;
}

/* A value from a layer's table: a string's bytes, or an integer. */
struct value { const uint8_t *s; size_t n; int64_t i; int is_string, is_int; };

static int read_value(struct pb v, struct value *out)
{
    uint32_t f, w;
    uint64_t x;
    struct pb sub;

    memset(out, 0, sizeof *out);

    while (pb_next(&v, &f, &w, &x, &sub)) {
        if (f == 1 && w == 2) {
            out->s = sub.p, out->n = (size_t)(sub.end - sub.p), out->is_string = 1;
        } else if ((f == 4 || f == 5) && w == 0) {
            out->i = (int64_t)x, out->is_int = 1;
        } else if (f == 6 && w == 0) {
            out->i = (int64_t)((x >> 1) ^ (~(x & 1) + 1)), out->is_int = 1;
        } else if (f == 7 && w == 0) {
            out->i = (int64_t)x, out->is_int = 1;
        }
    }

    return 1;
}

static int is_key(struct pb k, const char *name)
{
    size_t n = strlen(name);

    return (size_t)(k.end - k.p) == n && memcmp(k.p, name, n) == 0;
}

/* Offsets into the pool until decoding is over; -1 for none. */
struct pending { long klass, subclass, name; };

static int geometry(struct mvt_tile *t, struct mvt_feature *f, struct pb g)
{
    int32_t x = 0, y = 0;
    uint64_t cmd;

    f->part0 = (uint32_t)t->nparts, f->nparts = 0;

    while (g.p < g.end) {
        uint32_t id, count;

        if (!pb_varint(&g, &cmd)) return MVT_DAMAGED;

        id = (uint32_t)(cmd & 7), count = (uint32_t)(cmd >> 3);

        if (id == 7) {              /* close the ring: nothing to store */
            continue;
        }

        if (id != 1 && id != 2) return MVT_DAMAGED;

        for (uint32_t i = 0; i < count; i++) {
            uint64_t dx, dy;

            if (!pb_varint(&g, &dx) || !pb_varint(&g, &dy)) return MVT_DAMAGED;

            x += (int32_t)((dx >> 1) ^ (~(dx & 1) + 1));
            y += (int32_t)((dy >> 1) ^ (~(dy & 1) + 1));

            /* A move starts a part: a line, a ring, or - for points - each
             * point its own. */
            if (id == 1) {
                if (!grow((void **)&t->parts, &t->cap_p, t->nparts + 1, sizeof *t->parts)) {
                    return MVT_NO_MEMORY;
                }

                t->parts[t->nparts].first = (uint32_t)t->npoints;
                t->parts[t->nparts].count = 0;
                t->nparts++;
                f->nparts++;
            } else if (f->nparts == 0) {
                return MVT_DAMAGED;     /* a line to, with nothing moved to */
            }

            if (!grow((void **)&t->xy, &t->cap_xy, 2 * (t->npoints + 1), sizeof *t->xy)) {
                return MVT_NO_MEMORY;
            }

            t->xy[2 * t->npoints] = (float)x;
            t->xy[2 * t->npoints + 1] = (float)y;
            t->npoints++;
            t->parts[t->nparts - 1].count++;
        }
    }

    return MVT_OK;
}

static int layer(struct mvt_tile *t, struct pb l, struct pending **pend, size_t *cap_pend)
{
    struct pb *keys = NULL, sub;
    struct value *values = NULL;
    size_t nkeys = 0, nvalues = 0, cap_v = 0, cap_k = 0;
    uint32_t f, w;
    uint64_t v;
    int kind = L_OTHER;
    uint32_t extent = 4096;
    struct pb scan = l;
    int result = MVT_OK;

    /* First pass: the name, the extent, the tables. */
    while (pb_next(&scan, &f, &w, &v, &sub)) {
        if (f == 1 && w == 2) {
            kind = mvt_layer_of((const char *)sub.p, (size_t)(sub.end - sub.p));
        } else if (f == 3 && w == 2) {
            if (!grow((void **)&keys, &cap_k, nkeys + 1, sizeof *keys)) {
                free(values);
                return MVT_NO_MEMORY;
            }
            keys[nkeys++] = sub;
        } else if (f == 4 && w == 2) {
            if (!grow((void **)&values, &cap_v, nvalues + 1, sizeof *values)) {
                free(keys);
                return MVT_NO_MEMORY;
            }
            read_value(sub, &values[nvalues++]);
        } else if (f == 5 && w == 0) {
            extent = (uint32_t)v;
        }
    }

    if (scan.p != scan.end) {
        free(keys);
        free(values);
        return MVT_DAMAGED;
    }

    /* Layers of one tile share an extent in practice; the first is kept. */
    if (t->extent == 0) t->extent = extent ? extent : 4096;

    /* Second pass: the features. */
    while (result == MVT_OK && pb_next(&l, &f, &w, &v, &sub)) {
        struct mvt_feature *ft;
        struct pending *pd;
        struct pb fb = sub, gsub, geom = { 0 };
        int have_geom = 0;

        if (f != 2 || w != 2) continue;

        if (!grow((void **)&t->features, &t->cap_f, t->nfeatures + 1, sizeof *t->features)
            || !grow((void **)pend, cap_pend, t->nfeatures + 1, sizeof **pend)) {
            result = MVT_NO_MEMORY;
            break;
        }

        ft = &t->features[t->nfeatures];
        pd = &(*pend)[t->nfeatures];
        memset(ft, 0, sizeof *ft);
        pd->klass = pd->subclass = pd->name = -1;
        ft->layer = (uint8_t)kind;

        while (pb_next(&fb, &f, &w, &v, &gsub)) {
            if (f == 3 && w == 0) {
                ft->type = (uint8_t)v;
            } else if (f == 4 && w == 2) {
                geom = gsub, have_geom = 1;
            } else if (f == 2 && w == 2) {
                /* The tags: pairs of a key's index and a value's. */
                struct pb tags = gsub;
                uint64_t ki, vi;
                int named = 0;      /* which of its names was kept: 1 to 3 */

                while (tags.p < tags.end) {
                    if (!pb_varint(&tags, &ki) || !pb_varint(&tags, &vi)) {
                        result = MVT_DAMAGED;
                        break;
                    }

                    if (ki >= nkeys || vi >= nvalues) continue;

                    struct pb k = keys[ki];
                    struct value *val = &values[vi];

                    if (val->is_string) {
                        long *slot = is_key(k, "class") ? &pd->klass
                                   : is_key(k, "subclass") ? &pd->subclass : NULL;

                        /* A place's name in the Latin alphabet, which the
                         * system's faces draw - English, else its Latin
                         * spelling, else the name as the place writes it
                         * (OpenMapTiles carries all three; "Ελλάδα" as
                         * that would be "????" on the screen). */
                        int rank = is_key(k, "name:en") || is_key(k, "name_en") ? 3
                                 : is_key(k, "name:latin") ? 2
                                 : is_key(k, "name") ? 1 : 0;

                        if (rank > named) {
                            named = rank;
                            pd->name = keep(t, val->s, val->n);
                        } else if (slot != NULL) {
                            *slot = keep(t, val->s, val->n);
                        } else if (is_key(k, "brunnel")) {
                            ft->brunnel = (val->n == 6 && memcmp(val->s, "bridge", 6) == 0) ? 1
                                        : (val->n == 6 && memcmp(val->s, "tunnel", 6) == 0) ? 2 : 0;
                        }
                    } else if (val->is_int && is_key(k, "rank")) {
                        ft->rank = (int32_t)val->i;
                    }
                }
            }
        }

        if (result != MVT_OK) break;

        if (have_geom && ft->type >= MVT_POINT && ft->type <= MVT_POLYGON) {
            result = geometry(t, ft, geom);

            if (result == MVT_OK && ft->nparts > 0) t->nfeatures++;
        }
    }

    free(keys);
    free(values);
    return result;
}

int mvt_decode(struct mvt_tile *t, const uint8_t *data, size_t len)
{
    struct pb b = { data, data + len }, sub;
    struct pending *pend = NULL;
    size_t cap_pend = 0;
    uint32_t f, w;
    uint64_t v;
    int result = MVT_OK;

    memset(t, 0, sizeof *t);

    /* An empty string at 0, for a feature without a class. */
    if (keep(t, (const uint8_t *)"", 0) < 0) return MVT_NO_MEMORY;

    while (result == MVT_OK && pb_next(&b, &f, &w, &v, &sub)) {
        if (f == 3 && w == 2) result = layer(t, sub, &pend, &cap_pend);
    }

    if (result == MVT_OK && b.p != b.end) result = MVT_DAMAGED;

    /* The pool is where it will stay: offsets into it become pointers. */
    for (size_t i = 0; result == MVT_OK && i < t->nfeatures; i++) {
        struct mvt_feature *ft = &t->features[i];

        ft->klass = t->strings + (pend[i].klass > 0 ? pend[i].klass : 0);
        ft->subclass = t->strings + (pend[i].subclass > 0 ? pend[i].subclass : 0);
        ft->name = pend[i].name >= 0 ? t->strings + pend[i].name : NULL;
    }

    free(pend);

    if (t->extent == 0) t->extent = 4096;

    if (result != MVT_OK) mvt_free(t);

    return result;
}

void mvt_free(struct mvt_tile *t)
{
    free(t->features);
    free(t->parts);
    free(t->xy);
    free(t->strings);
    memset(t, 0, sizeof *t);
}
