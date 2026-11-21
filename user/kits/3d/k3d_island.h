/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **An invented island** (`docs/flight.md` F5j): a ground to fly over
 * when there is nothing else - no network, and nothing in the cache - so
 * a window that draws a terrain always has somewhere to be.
 *
 * About fourteen kilometres across, centred on the frame's origin: hills
 * to a few hundred metres, a coast of sand, grass and woods, rock where
 * it is high or steep, and the sea around it thirty metres deep. Value
 * noise over the frame's metres, so the same place is always the same
 * height and colour, whichever tile or zoom asks - which is what keeps
 * one tile's edge the same as its neighbour's.
 *
 * Made a tile at a time, as the real tiles come, so the terrain takes it
 * through the same door: heights like an elevation tile's, a picture
 * like an imagery tile's. No Lua, no GPU; `tools/test_terrain.c` holds
 * it on the host.
 */

#ifndef K3D_ISLAND_H
#define K3D_ISLAND_H

#include "k3d_terrain.h"

#include <stdint.h>

/* The island's metres at a place in the frame (x east, z south): below
 * nought is the sea's floor. */
float k3d_island_height(double x, double z);

/* Its colour there, 0xAARRGGBB: the sea's by its depth, then sand, grass,
 * woods and rock by height and slope. */
uint32_t k3d_island_colour(double x, double z);

/* A tile's heights, `side` by `side` samples at their centres, rows north
 * to south - the ground's, the sea's surface at nought - and the lowest
 * and highest of them. */
void k3d_island_heights(const struct k3d_place *p, struct k3d_tile t, unsigned side,
                        float *metres, float *lowest, float *highest);

/* A tile's picture, `side` by `side` pixels, packed. */
void k3d_island_picture(const struct k3d_place *p, struct k3d_tile t, unsigned side,
                        uint32_t *pixels);

#endif
