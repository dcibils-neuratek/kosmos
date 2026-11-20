/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Heights from an elevation tile** (`docs/flight.md` F2): a grid of
 * metres above the sea, from a terrarium tile's pixels - Mapzen's encoding,
 * which AWS serves for the whole world:
 *
 *     height = R x 256 + G + B / 256 - 32768 metres
 *
 * The PNG is decoded by gfx, the system's one PNG decoder, and its pixels
 * come here; what comes back is floats in C, never a Lua table (`gfx.md`
 * 19.1 - a tile is 65,536 of them). The terrain (F4) reads the grid from
 * C; Lua asks it for a height at a time.
 *
 * Nothing here knows Lua or a surface, so `tools/test_map.c` holds it to
 * known heights on the host.
 */

#ifndef KOSMOS_KITS_MAP_ELEVATION_H
#define KOSMOS_KITS_MAP_ELEVATION_H

#include <stdbool.h>
#include <stdint.h>

struct map_heights {
    unsigned width, height;
    float   *metres;            /* width x height, rows north to south */
    float    lowest, highest;
};

/* One terrarium pixel, 0xAARRGGBB as gfx keeps it, to metres. */
float map_terrarium(uint32_t pixel);

/* A tile's pixels - `pitch` bytes a row - to heights. False, and `out`
 * left empty, when memory is out or the tile is smaller than 2 by 2. */
bool map_heights_terrarium(struct map_heights *out, const uint32_t *pixels,
                           unsigned width, unsigned height, unsigned pitch);

/* The height at a sample. */
float map_heights_at(const struct map_heights *h, unsigned x, unsigned y);

/* The height at `u`, `v` across the tile, 0 to 1 west to east and north to
 * south: between the four samples around it, a sample's value at its
 * centre, and the edge's beyond the outer centres. */
float map_heights_sample(const struct map_heights *h, double u, double v);

void map_heights_free(struct map_heights *h);

#endif
