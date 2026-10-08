/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A vector tile drawn in a style** (`docs/maps.md` M3) - the busiest path
 * Maps has: every visible tile, every frame of a drag.
 *
 * **One path a rule.** Every feature a rule takes goes into one gfx path -
 * rings filled, lines stroked at the rule's width for this zoom - and the
 * path is painted once. A tile of a city at zoom 16 is a few thousand
 * features under some twenty rules: twenty passes over the tile's pixels,
 * not thousands.
 *
 * **The tile's box is the path's box**, cut to the caller's clip. A tile's
 * features run a little past its edge (the buffer every vector tile has),
 * and drawing only inside the tile is what makes neighbours meet without a
 * seam: a road's round end at the buffer is cut at the edge, where the next
 * tile's part of the same road begins.
 *
 * **Matching is done once a tile and a style meet**, not every frame: for
 * each rule, the indices of the features it takes, kept with the tile and
 * made again only when the style changes (`version`).
 */

#include "mapdraw.h"

#include <stdlib.h>
#include <string.h>

void map_matches_free(struct map_matches *m)
{
    free(m->list);
    free(m->first);
    memset(m, 0, sizeof *m);
}

float map_width(const struct map_rule *r, float zoom)
{
    if (r->nstops <= 0) return 1.0f;
    if (zoom <= r->stops[0]) return r->stops[1];

    for (int i = 1; i < r->nstops; i++) {
        float z0 = r->stops[2 * i - 2], w0 = r->stops[2 * i - 1];
        float z1 = r->stops[2 * i], w1 = r->stops[2 * i + 1];

        if (zoom <= z1) {
            float f = z1 > z0 ? (zoom - z0) / (z1 - z0) : 1.0f;

            return w0 + (w1 - w0) * f;
        }
    }

    return r->stops[2 * r->nstops - 1];
}

/* Whether `klass` is one of a rule's comma-separated classes - or the rule
 * names none, and takes every one. */
static int takes(const struct map_rule *r, const char *klass)
{
    const char *c = r->classes;
    size_t n = strlen(klass);

    if (*c == '\0') return 1;

    while (*c) {
        const char *end = strchr(c, ',');
        size_t len = end ? (size_t)(end - c) : strlen(c);

        if (len == n && memcmp(c, klass, n) == 0) return 1;

        if (!end) break;

        c = end + 1;
    }

    return 0;
}

static int match(const struct mvt_tile *t, struct map_matches *m, const struct map_style *s)
{
    size_t n = 0;

    if (m->version == s->version && m->first != NULL) return 0;

    if (s->nrules + 1 > m->cap_first) {
        uint32_t *f = realloc(m->first, (s->nrules + 1) * sizeof *f);

        if (f == NULL) return -1;

        m->first = f, m->cap_first = s->nrules + 1;
    }

    /* Twice: counting, then filling, so the list is one allocation. */
    for (int pass = 0; pass < 2; pass++) {
        n = 0;

        for (size_t r = 0; r < s->nrules; r++) {
            const struct map_rule *rule = &s->rules[r];
            int want = rule->kind == MAP_FILL ? MVT_POLYGON : 0;

            if (pass == 1) m->first[r] = (uint32_t)n;

            for (size_t i = 0; i < t->nfeatures; i++) {
                const struct mvt_feature *f = &t->features[i];

                if (f->layer != rule->layer) continue;
                if (want && f->type != want) continue;
                if (!want && f->type == MVT_POINT) continue;
                if (!takes(rule, f->klass)) continue;

                if (pass == 1) m->list[n] = (uint32_t)i;
                n++;
            }
        }

        if (pass == 0 && n > m->cap_list) {
            uint32_t *l = realloc(m->list, (n ? n : 1) * sizeof *l);

            if (l == NULL) return -1;

            m->list = l, m->cap_list = n;
        }
    }

    m->first[s->nrules] = (uint32_t)n;
    m->version = s->version;
    return 0;
}

/* A part's points, moved from the tile's units onto the pixels. */
static float *placed;
static size_t placed_cap;

static const float *place(const struct mvt_tile *t, const struct mvt_part *pt,
                          float ox, float oy, float k)
{
    if (2 * (size_t)pt->count > placed_cap) {
        size_t cap = placed_cap ? placed_cap : 256;
        float *grown;

        while (cap < 2 * (size_t)pt->count) cap *= 2;

        if ((grown = realloc(placed, cap * sizeof *grown)) == NULL) return NULL;

        placed = grown, placed_cap = cap;
    }

    for (uint32_t i = 0; i < pt->count; i++) {
        placed[2 * i] = ox + t->xy[2 * (pt->first + i)] * k;
        placed[2 * i + 1] = oy + t->xy[2 * (pt->first + i) + 1] * k;
    }

    return placed;
}

int map_draw(const struct mvt_tile *t, struct map_matches *m, const struct map_style *s,
             struct gfx_path *p, uint32_t *pixels, unsigned pitch, long w, long h,
             float ox, float oy, float size, float zoom,
             long cx0, long cy0, long cx1, long cy1)
{
    long bx0 = (long)ox, by0 = (long)oy;
    long bx1 = (long)(ox + size + 0.999f), by1 = (long)(oy + size + 0.999f);
    float k = size / (float)(t->extent ? t->extent : 4096);
    int drew = 0;

    if (match(t, m, s) != 0) return 0;

    /* The tile's box, cut to the clip and to the pixels. */
    if (bx0 < cx0) bx0 = cx0;
    if (by0 < cy0) by0 = cy0;
    if (bx1 > cx1) bx1 = cx1;
    if (by1 > cy1) by1 = cy1;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > w) bx1 = w;
    if (by1 > h) by1 = h;

    if (bx1 <= bx0 || by1 <= by0) return 0;

    for (size_t r = 0; r < s->nrules; r++) {
        const struct map_rule *rule = &s->rules[r];
        uint32_t a = m->first[r], b = m->first[r + 1];
        float width;

        if (a == b || zoom < rule->minzoom || zoom >= rule->maxzoom) continue;

        width = rule->kind == MAP_LINE ? map_width(rule, zoom) : 0.0f;

        if (rule->kind == MAP_LINE && width <= 0.05f) continue;

        if (gfx_path_begin(p, bx0, by0, bx1 - bx0, by1 - by0) != 0) return drew;

        for (uint32_t i = a; i < b; i++) {
            const struct mvt_feature *f = &t->features[m->list[i]];

            for (uint32_t pi = 0; pi < f->nparts; pi++) {
                const struct mvt_part *pt = &t->parts[f->part0 + pi];
                const float *xy = place(t, pt, ox, oy, k);

                if (xy == NULL) continue;

                /* A line rule over a polygon strokes its outline - a park's
                 * edge, a building's - as it strokes a road. */
                if (rule->kind == MAP_FILL) {
                    gfx_path_ring(p, xy, pt->count);
                } else {
                    gfx_path_stroke(p, xy, pt->count, width);
                }
            }
        }

        gfx_path_paint(p, (uint32_t *)((uint8_t *)pixels + (size_t)by0 * pitch) + bx0,
                       pitch, rule->colour);
        drew++;
    }

    return drew;
}
