/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A Mapbox Vector Tile, decoded (`docs/maps.md` M2): its layers' features,
 * each with what a map needs of it - its layer, kind, class, name and rank
 * - and its geometry as points in the tile's own units. `mvt.c` is the
 * argument; this is the shape the Map Kit's drawing and the host test
 * compile against.
 *
 * A decoded tile owns everything in it, the strings included: the bytes it
 * was decoded from may go once it is.
 */

#ifndef KOSMOS_MAP_MVT_H
#define KOSMOS_MAP_MVT_H

#include <stddef.h>
#include <stdint.h>

/* The OpenMapTiles layers this draws, and everything else as `other`. */
enum mvt_layer {
    L_OTHER, L_WATER, L_WATERWAY, L_LANDCOVER, L_LANDUSE, L_PARK, L_BUILDING,
    L_TRANSPORTATION, L_TRANSPORTATION_NAME, L_PLACE, L_WATER_NAME, L_POI,
    L_BOUNDARY, L_AEROWAY, L_COUNT
};

enum mvt_type { MVT_UNKNOWN = 0, MVT_POINT = 1, MVT_LINE = 2, MVT_POLYGON = 3 };

struct mvt_feature {
    uint8_t  layer;         /* enum mvt_layer */
    uint8_t  type;          /* enum mvt_type */
    uint8_t  brunnel;       /* 1 bridge, 2 tunnel, 0 neither */
    int32_t  rank;          /* `rank`, 0 when it has none */
    const char *klass;      /* `class`, "" when it has none */
    const char *subclass;   /* `subclass`, "" when it has none */
    const char *name;       /* `name`, NULL when it has none */
    uint32_t part0, nparts; /* its parts: lines, rings, or points */
};

struct mvt_part {
    uint32_t first, count;  /* its points in `xy`, as pairs */
};

struct mvt_tile {
    uint32_t extent;        /* the units of `xy` across the tile: 4096 */
    struct mvt_feature *features;
    size_t              nfeatures;
    struct mvt_part    *parts;
    size_t              nparts;
    float              *xy;     /* x, y pairs in tile units */
    size_t              npoints;
    char               *strings;
    size_t              nstrings;
    size_t              cap_f, cap_p, cap_xy, cap_s;
};

enum {
    MVT_OK = 0,
    MVT_DAMAGED = -1,       /* not a vector tile, or one cut short */
    MVT_NO_MEMORY = -2
};

/* `len` bytes of protobuf at `data` (inflated already) into `t`, which
 * starts empty and is the caller's to `mvt_free`. */
int  mvt_decode(struct mvt_tile *t, const uint8_t *data, size_t len);
void mvt_free(struct mvt_tile *t);

/* A layer's name as `enum mvt_layer`. */
int  mvt_layer_of(const char *name, size_t len);

#endif
