/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * ufbx's switches, as Kosmos builds it (`runtime/upstream/ufbx/`).
 *
 * `ufbx.h` includes this first when `UFBX_CONFIG_HEADER` names it, so the
 * vendored `ufbx.c` and our `k3d_fbx.c` are compiled with the same
 * configuration and neither is edited - the same arrangement
 * `record_config.h` makes for minih264.
 */
#ifndef K3D_UFBX_H
#define K3D_UFBX_H

/* The bytes come from Cafesa3D, which read the file; ufbx opens nothing. */
#define UFBX_NO_STDIO

/* OBJ has a translator of its own (`/lib/translators/obj.lua`). */
#define UFBX_NO_FORMAT_OBJ

/* What a model for Cafesa3D does not need: vertex caches, animation
 * baked into keys, subdivision surfaces, NURBS made into triangles, and
 * skinning worked out - a character comes in as it was modelled. */
#define UFBX_NO_GEOMETRY_CACHE
#define UFBX_NO_ANIMATION_BAKING
#define UFBX_NO_SUBDIVISION
#define UFBX_NO_TESSELLATION
#define UFBX_NO_SKINNING_EVALUATION

/*
 * ufbx's own assertions, kept: a failure stops the process, as any failed
 * assertion here does. Not the libc's `assert`, which calls `panic` - and
 * ufbx has a parameter named `panic` that such a macro would call instead.
 */
void k3d_ufbx_failed(const char *what) __attribute__((noreturn));

#define ufbx_assert(cond) ((cond) ? (void)0 : k3d_ufbx_failed(#cond))

#endif
