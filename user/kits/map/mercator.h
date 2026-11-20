/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Web Mercator**, the projection every tiled map uses (`docs/flight.md`
 * F2): a place's position across the world, the tile it falls in, a
 * tile's edges, and how many metres a pixel covers.
 *
 * Maps did the tile and metre arithmetic itself, in Lua, and Flight would
 * have done it again: so it is here, once, with no Lua and no hardware in
 * it, and `tools/test_map.c` holds it to known places on the host.
 *
 * Positions across the world are 0 to 1, west to east and north to south.
 * A latitude beyond 85.0511 degrees, where the square world ends, is taken
 * as that latitude.
 */

#ifndef KOSMOS_KITS_MAP_MERCATOR_H
#define KOSMOS_KITS_MAP_MERCATOR_H

#include <stdint.h>

#define MAP_LAT_MOST 85.05112878

/* The equator's length, metres: 2 pi times WGS 84's radius. */
#define MAP_EQUATOR_METRES 40075016.68557849

void map_project(double lon, double lat, double *x, double *y);
void map_unproject(double x, double y, double *lon, double *lat);

/* The tile at zoom `z` (0 to 30) a place is in, kept inside the world. */
void map_tile_of(double lon, double lat, unsigned z, uint32_t *tx, uint32_t *ty);

/* A tile's edges, in degrees. */
void map_tile_bounds(unsigned z, uint32_t x, uint32_t y,
                     double *west, double *south, double *east, double *north);

/* The ground a pixel covers at a latitude, for tiles `size` pixels a side
 * at zoom `z`: metres, east to west (and north to south: the projection
 * keeps shapes, so both are the same there). */
double map_metres_per_pixel(double lat, unsigned z, unsigned size);

#endif
