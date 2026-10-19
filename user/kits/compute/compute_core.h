/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Compute Kit's own arithmetic** (`docs/compute.md` C3): the shape a
 * buffer is given in the GPU's table, and a run's message built and
 * checked - the part of the kit with no process in it, so the Mac holds it
 * to `gpuproto.h` (`tools/test_compute.c`) and the kit on Kosmos sends only
 * what passed here.
 */

#ifndef KOSMOS_COMPUTE_CORE_H
#define KOSMOS_COMPUTE_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "gpuproto.h"

/*
 * How a buffer is entered in the GPU's table: `width` by `height` four-byte
 * pixels at `pitch`, in `pages` pages. Bytes: rows of a page, as many as
 * hold `bytes` - a kernel sees whole rows, so the last page's tail too.
 * A picture: its rows at a pitch of whole 64-byte lines, as the data port
 * reads them. NULL, or why not.
 */
struct compute_shape {
    uint32_t width, height, pitch, pages;
};

const char *compute_bytes_shape(size_t bytes, struct compute_shape *out);
const char *compute_picture_shape(uint32_t width, uint32_t height, struct compute_shape *out);

/* A view's name - "bytes", "bgra", "r8", "r32f" - as GPU_VIEW_*, or -1. */
int compute_view(const char *name);

/*
 * A run's message: the kernel, SIMD 8 or 16, a grid, up to eight buffers
 * by id and view, and up to 256 bytes of constants, padded to whole
 * registers of 32. NULL, or why not - a sentence for a person.
 */
struct compute_binding {
    uint32_t id, view;
};

const char *compute_pack_run(struct gpu_run *out, uint32_t kernel, uint32_t simd,
                             uint32_t groups_x, uint32_t groups_y,
                             const struct compute_binding *buffers, uint32_t nbuffers,
                             const void *constants, size_t nconstants);

/* A step of a list of runs: its kernel, lanes, grid and number. */
struct compute_step {
    uint32_t kernel, simd, groups_x, groups_y, step;
};

/* A list of runs (GPU_OP_RUNS) packed as the driver reads it, or why not:
 * the steps sharing the buffers and the constants, each step's number at
 * `step_at` of them. */
const char *compute_pack_runs(struct gpu_runs *out, const struct compute_binding *buffers,
                              uint32_t nbuffers, const void *constants, size_t nconstants,
                              uint32_t step_at, const struct compute_step *steps, uint32_t nsteps);

#endif
