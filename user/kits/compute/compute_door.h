/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Compute Kit's door for another kit in C**, as `gfx_draw.h` is
 * gfx's: a buffer's bytes, for a kit that fills one - the 3D Kit laying a
 * scene into the tracer's buffers (`docs/cafesa3d-gpu.md` R6) - without
 * the bytes passing through Lua, where they would be a string a megabyte
 * long made only to be copied again.
 */

#ifndef KOSMOS_COMPUTE_DOOR_H
#define KOSMOS_COMPUTE_DOOR_H

#include <stddef.h>

struct lua_State;

/*
 * The compute buffer at `index` on the stack: its bytes, mapped here, and
 * how many through `size` - what the program asked for, which the kit's
 * writes are held to. A Lua error, not a pointer, for anything that is
 * not a live compute buffer.
 */
void *compute_buffer_bytes(struct lua_State *L, int index, size_t *size);

#endif
