/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Web Mercator (`mercator.h`): the projection, tiles and metres.
 */

#include <math.h>

#include "mercator.h"

static double clamp_lat(double lat)
{
    if (lat > MAP_LAT_MOST) return MAP_LAT_MOST;
    if (lat < -MAP_LAT_MOST) return -MAP_LAT_MOST;
    return lat;
}

void map_project(double lon, double lat, double *x, double *y)
{
    double r = clamp_lat(lat) * M_PI / 180.0;

    *x = (lon + 180.0) / 360.0;
    *y = (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0;
}

void map_unproject(double x, double y, double *lon, double *lat)
{
    *lon = x * 360.0 - 180.0;
    *lat = atan(sinh(M_PI * (1.0 - 2.0 * y))) * 180.0 / M_PI;
}

static uint32_t inside(double t, double n)
{
    double v = floor(t * n);

    if (v < 0.0) return 0;
    if (v > n - 1.0) return (uint32_t)(n - 1.0);
    return (uint32_t)v;
}

void map_tile_of(double lon, double lat, unsigned z, uint32_t *tx, uint32_t *ty)
{
    double x, y, n;

    if (z > 30) z = 30;

    n = ldexp(1.0, (int)z);
    map_project(lon, lat, &x, &y);
    *tx = inside(x, n);
    *ty = inside(y, n);
}

void map_tile_bounds(unsigned z, uint32_t x, uint32_t y,
                     double *west, double *south, double *east, double *north)
{
    double n = ldexp(1.0, (int)(z > 30 ? 30 : z));

    map_unproject(x / n, (y + 1.0) / n, west, south);
    map_unproject((x + 1.0) / n, y / n, east, north);
}

double map_metres_per_pixel(double lat, unsigned z, unsigned size)
{
    double n = ldexp(1.0, (int)(z > 30 ? 30 : z));

    if (size == 0) size = 256;

    return MAP_EQUATOR_METRES * cos(clamp_lat(lat) * M_PI / 180.0) / (size * n);
}
