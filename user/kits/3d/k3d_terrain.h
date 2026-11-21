/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A terrain** (`docs/flight.md` F4): the ground under a place, as chunks
 * of a height field at the detail the eye's distance asks for.
 *
 *     a place         the world's frame: metres from an origin on the
 *                     map, x to the east, y up, z to the south;
 *     a choice        which chunks to draw from where the eye is - the
 *                     leaves of a quadtree over the map's tiles, deeper
 *                     nearer the eye;
 *     a chunk         one tile's ground as a mesh: 65 by 65 samples of its
 *                     heights, each with its normal and where it is on the
 *                     tile's imagery, and a skirt hanging under its edges
 *                     so a coarser neighbour leaves no crack.
 *
 * Heights come from a function the caller hands over - the Map Kit's
 * elevation tiles in Flight, a plane or a hill in a test - so nothing here
 * knows a tile's bytes, Lua, or the GPU, and `tools/test_terrain.c` holds
 * it on the host. Drawing a chunk is the renderer's (`k3d_vk.c`).
 *
 * **A shared edge is the same bits on both sides.** A sample's place on
 * the map is (tile x 64 + i) / (64 x 2^zoom) - integers, divided once,
 * the same whichever of two neighbouring chunks asks - and its normal is
 * from the samples either side of it, beyond the edge too, so both chunks
 * light it alike. The depth buffer and the light see no seam.
 *
 * The frame is flat: metres are the Mercator scale at the origin's
 * latitude, which is right to a part in ten thousand ten kilometres away
 * and is what a flight over a city needs. Floats hold a position to a few
 * millimetres there.
 */

#ifndef K3D_TERRAIN_H
#define K3D_TERRAIN_H

#include <stdint.h>

/* Samples along a chunk's side, and the vertices and indices one takes:
 * its grid, then a skirt of one row under each of its four edges. */
#define K3D_CHUNK_SIDE      65
#define K3D_CHUNK_VERTICES  (K3D_CHUNK_SIDE * K3D_CHUNK_SIDE + 4 * K3D_CHUNK_SIDE)
#define K3D_CHUNK_INDICES   (6 * (K3D_CHUNK_SIDE - 1) * (K3D_CHUNK_SIDE - 1) + \
                             4 * 6 * (K3D_CHUNK_SIDE - 1))

struct k3d_place {
    double ox, oy;              /* the origin, across the world 0 to 1 */
    double metres;              /* metres to one whole world, at its latitude */
};

struct k3d_tile {
    uint32_t x, y;
    uint8_t z;
};

/* What decides the chunks: the roots are the tiles at zoom `root` within
 * `radius` of the eye's, each way; a chunk splits in four while the eye is
 * nearer than `split` times its side, down to zoom `deepest`. */
struct k3d_choice {
    unsigned root, deepest, radius;
    double split;
};

/* The metres at a place on the map, across the world 0 to 1. */
typedef float (*k3d_height_fn)(void *context, double mx, double my);

struct k3d_terrain_vertex {
    float position[3];
    float normal[3];
    float uv[2];                /* across the tile, 0 to 1 west to east,
                                 * north to south: its imagery's */
};

/* The frame whose origin is at `lon`, `lat`. */
void k3d_place_at(struct k3d_place *p, double lon, double lat);

/* A place across the world, 0 to 1, in the frame: metres east and south. */
void k3d_place_world(const struct k3d_place *p, double mx, double my, double *x, double *z);

/* A tile's side in the frame, metres. */
double k3d_tile_side(const struct k3d_place *p, unsigned z);

/* The chunks to draw for an eye at (x, y, z) in the frame, at most `most`
 * of them, nearest the eye split first: how many were written. They tile
 * the roots exactly, with no gap and no overlap, whatever `most` is - a
 * chunk is only split when its four fit - and none is deeper than
 * `deepest`. 0 when `most` is smaller than the number of roots. */
unsigned k3d_terrain_choose(const struct k3d_place *p, const struct k3d_choice *c,
                            const double eye[3], struct k3d_tile *out, unsigned most);

/* A tile's ground in the frame: K3D_CHUNK_VERTICES vertices and
 * K3D_CHUNK_INDICES indices, three to a triangle, wound counter-clockwise
 * seen from above; the skirt hangs `skirt` metres under the edges. Its
 * lowest and highest height go to `lowest` and `highest` if they are not
 * NULL. */
void k3d_terrain_chunk(const struct k3d_place *p, struct k3d_tile t, k3d_height_fn height,
                       void *context, float skirt, struct k3d_terrain_vertex *vertices,
                       uint32_t *indices, float *lowest, float *highest);

/*
 * **A flat** (`docs/flight.md` F8): a runway's ground, level across and
 * straight along - the elevation tiles are 20 m a sample at best and put a
 * runway on whatever the hill under it does. A segment from `a` to `b`
 * across the world (0 to 1, as `k3d_height_fn` takes), the heights at its
 * ends, and two half-widths in the same units: inside `half` of the
 * segment the ground is the segment's height, the line between its ends;
 * over the next `margin` it blends back to the tiles' with a smoothstep;
 * beyond, the tiles' own. Its ends are round, which a runway's stopway
 * does not mind.
 */
struct k3d_flat {
    double ax, ay, bx, by;
    double half, margin;
    float ha, hb;
};

/* The height `h` the tiles give at (mx, my), as `n` flats leave it - each
 * in turn, so where two meet the later one's is the ground. */
float k3d_flatten(const struct k3d_flat *flats, unsigned n, double mx, double my, float h);

#endif
