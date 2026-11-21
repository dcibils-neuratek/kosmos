/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A footprint made solid** (`docs/flight.md` F6, buildings B2): a
 * polygon with holes, in metres on the ground, made into a roof - its
 * triangles by ear clipping, the holes first joined to the outer ring by a
 * bridge each - and its rings stood up into walls between two heights.
 *
 * The plane is the world's ground, x east and z south, y up, as the 3D
 * Kit's terrain lays it; a ring is its points in order, either way round,
 * with or without its first point repeated at the end. What comes back is
 * appended to the caller's arrays, so a tile's thousands of buildings
 * grow one pair of arrays rather than a pair each.
 *
 * Plain C, no allocation but the caller's arrays growing and one scratch
 * array the call frees, so a host test holds it to areas and windings and
 * the terrain's builder thread calls it as it is. Flight's buildings are
 * its first user; Maps' 3D view and Cafesa3D's import of a plan want the
 * same.
 */
#ifndef KOSMOS_K3D_EXTRUDE_H
#define KOSMOS_K3D_EXTRUDE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A ring: `n` points, x and z, two doubles each. */
struct k3d_ring {
    const double *xz;
    unsigned n;
};

/* A vertex as the 3D Kit's models take it (`k3d_vk.h`): where, its
 * normal, and (u, v) - for a roof the point's own x and z, which whoever
 * draws it turns into a picture's coordinates. */
struct k3d_extrude_vertex {
    float position[3];
    float normal[3];
    float uv[2];
};

/* Arrays the calls append to; zeroed before the first, freed with
 * `k3d_extruded_free`. Roofs and walls apart, since they are drawn apart. */
struct k3d_extruded {
    struct k3d_extrude_vertex *roof;
    uint32_t *roof_index;
    size_t nroof, nroof_index, cap_roof, cap_roof_index;
    struct k3d_extrude_vertex *wall;
    uint32_t *wall_index;
    size_t nwall, nwall_index, cap_wall, cap_wall_index;
};

/* At most this many points in a footprint, its holes included: a larger
 * one is refused rather than clipped in quadratic time. */
#define K3D_EXTRUDE_MOST 4096

/*
 * The footprint `rings` - the first the outer, the rest holes - stood up
 * from `base` to `top` metres: a roof at `top` facing up, and walls facing
 * out of the solid. The rings' x and z are offset by `ox`, `oz`, so a
 * tile's buildings may be given in its own small numbers. False when it is
 * not one: fewer than three points, no area, too many points, or no memory
 * - and nothing of it is appended then.
 */
bool k3d_extrude(struct k3d_extruded *out, const struct k3d_ring *rings, unsigned nrings,
                 double ox, double oz, float base, float top);

void k3d_extruded_free(struct k3d_extruded *out);

#endif
