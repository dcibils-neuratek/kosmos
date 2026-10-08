/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A vector tile drawn in a style (`docs/maps.md` M3). `mapdraw.c` is the
 * argument; this is the shape the Map Kit's Lua door and the host test
 * compile against.
 *
 * **A style is a list of rules, drawn in order**: each takes the features
 * of one layer - of some of its classes, or all - and either fills them or
 * strokes them, in one colour, at a width that follows the zoom. A road's
 * casing is a rule before the road's own, wider and darker; nothing about
 * casings is known here. The rules come from Lua (`map.style`), so a look's
 * colours are data and not C.
 */

#ifndef KOSMOS_MAP_DRAW_H
#define KOSMOS_MAP_DRAW_H

#include <stddef.h>
#include <stdint.h>

#include "mvt.h"
#include "kits/gfx/path.h"

enum { MAP_FILL = 1, MAP_LINE = 2 };

#define MAP_CLASSES 160         /* a rule's classes, comma-separated */
#define MAP_STOPS   6           /* zoom and width pairs */

struct map_rule {
    uint8_t  layer;             /* enum mvt_layer */
    uint8_t  kind;              /* MAP_FILL or MAP_LINE */
    char     classes[MAP_CLASSES];  /* "" for every class */
    uint32_t colour;            /* 0xRRGGBB */
    float    minzoom, maxzoom;  /* drawn from, and before */
    float    stops[2 * MAP_STOPS];  /* zoom, width in pixels, ... */
    int      nstops;
};

struct map_style {
    uint32_t         background;
    struct map_rule *rules;
    size_t           nrules;
    uint32_t         version;   /* new each style, so a tile's matches know */
};

/*
 * Which features each rule takes, worked out once a tile and a style meet
 * and kept with the tile: `list[first[r] .. first[r + 1])` are rule r's.
 * So a frame of a drag walks lists and never compares a string.
 */
struct map_matches {
    uint32_t  version;          /* the style's they were made for; 0 none */
    uint32_t *list;
    uint32_t *first;            /* nrules + 1 */
    size_t    cap_list, cap_first;
};

void map_matches_free(struct map_matches *m);

/* A rule's width at `zoom`: between its stops, straight; held at the ends. */
float map_width(const struct map_rule *r, float zoom);

/*
 * Tile `t` drawn over pixels - `pitch` bytes a row, `w` by `h` of them -
 * with its top left at (ox, oy) and `size` pixels a side, at `zoom`, in
 * `style`, nothing outside [cx0, cx1) by [cy0, cy1). `p` is the path every
 * rule is drawn through, kept by the caller between tiles and frames.
 * Returns how many rules drew anything, for a test and the frame's log.
 */
int map_draw(const struct mvt_tile *t, struct map_matches *m, const struct map_style *s,
             struct gfx_path *p, uint32_t *pixels, unsigned pitch, long w, long h,
             float ox, float oy, float size, float zoom,
             long cx0, long cy0, long cx1, long cy1);

#endif
