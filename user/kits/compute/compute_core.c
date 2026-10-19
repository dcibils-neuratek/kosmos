/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Compute Kit's arithmetic (`compute_core.h`).
 */

#include <string.h>

#include "compute_core.h"

#define PAGE           4096u
#define MAX_SIDE       16384u   /* what GPU_OP_BUFFER_IMPORT takes */
#define MAX_PITCH      32768u

const char *compute_bytes_shape(size_t bytes, struct compute_shape *out)
{
    size_t rows = (bytes + PAGE - 1u) / PAGE;

    if (bytes == 0) {
        return "a buffer of no bytes";
    }

    if (rows > MAX_SIDE) {
        return "a buffer of more than 64 MB";
    }

    out->width = PAGE / 4u;
    out->pitch = PAGE;
    out->height = (uint32_t)rows;
    out->pages = (uint32_t)rows;
    return NULL;
}

const char *compute_picture_shape(uint32_t width, uint32_t height, struct compute_shape *out)
{
    uint32_t pitch;

    if (width == 0 || height == 0) {
        return "a picture of no pixels";
    }

    if (width > MAX_SIDE || height > MAX_SIDE) {
        return "a picture larger than 16384 pixels a side";
    }

    pitch = (width * 4u + 63u) & ~63u;

    if (pitch >= MAX_PITCH) {
        return "a picture wider than 8176 pixels";
    }

    out->width = width;
    out->height = height;
    out->pitch = pitch;
    out->pages = (uint32_t)(((uint64_t)pitch * height + PAGE - 1u) / PAGE);
    return NULL;
}

int compute_view(const char *name)
{
    static const char *const names[] = { "bytes", "bgra", "r8", "r32f", "table" };
    unsigned i;

    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (strcmp(name, names[i]) == 0) {
            return (int)i;
        }
    }

    return -1;
}

const char *compute_pack_run(struct gpu_run *out, uint32_t kernel, uint32_t simd,
                             uint32_t groups_x, uint32_t groups_y,
                             const struct compute_binding *buffers, uint32_t nbuffers,
                             const void *constants, size_t nconstants)
{
    uint32_t i;

    memset(out, 0, sizeof *out);

    if (kernel == 0) {
        return "no kernel";
    }

    if (simd != 8u && simd != 16u) {
        return "a thread is 8 or 16 lanes";
    }

    if (groups_x == 0 || groups_y == 0 || groups_x > 65535u || groups_y > 65535u) {
        return "a grid is 1 to 65535 thread groups each way";
    }

    if (nbuffers > GPU_RUN_BUFFERS) {
        return "a run binds eight buffers at most";
    }

    if (nconstants > GPU_RUN_CONSTANTS) {
        return "a run's constants are 256 bytes at most";
    }

    for (i = 0; i < nbuffers; i++) {
        if (buffers[i].id == 0 || buffers[i].view > GPU_VIEW_TABLE) {
            return "a buffer that is not one";
        }

        out->buffers[i].id = buffers[i].id;
        out->buffers[i].view = buffers[i].view;
    }

    out->op = GPU_OP_RUN;
    out->kernel = kernel;
    out->simd = simd;
    out->groups_x = groups_x;
    out->groups_y = groups_y;
    out->nbuffers = nbuffers;
    out->nconstants = (uint32_t)((nconstants + 31u) & ~(size_t)31u);

    if (nconstants > 0) {
        memcpy(out->constants, constants, nconstants);
    }

    return NULL;
}

const char *compute_pack_runs(struct gpu_runs *out, const struct compute_binding *buffers,
                              uint32_t nbuffers, const void *constants, size_t nconstants,
                              uint32_t step_at, const struct compute_step *steps, uint32_t nsteps)
{
    struct gpu_run one;
    uint32_t i;
    const char *why;

    memset(out, 0, sizeof *out);

    if (nsteps == 0 || nsteps > GPU_RUN_STEPS) {
        return "a list of runs is 1 to 32 steps";
    }

    /* The step's number is written into the constants, so it must be in
     * them: a whole word, below their end. */
    if (step_at % 4u != 0 || (size_t)step_at + 4u > ((nconstants + 31u) & ~(size_t)31u)) {
        return "a step's number goes in a word of the constants";
    }

    /* Each step held to what a run is held to, by packing it as one. */
    for (i = 0; i < nsteps; i++) {
        if ((why = compute_pack_run(&one, steps[i].kernel, steps[i].simd, steps[i].groups_x,
                                    steps[i].groups_y, buffers, nbuffers, constants,
                                    nconstants)) != NULL) {
            return why;
        }

        out->steps[i].kernel = steps[i].kernel;
        out->steps[i].simd = steps[i].simd;
        out->steps[i].groups_x = steps[i].groups_x;
        out->steps[i].groups_y = steps[i].groups_y;
        out->steps[i].step = steps[i].step;
    }

    out->op = GPU_OP_RUNS;
    out->nbuffers = one.nbuffers;
    memcpy(out->buffers, one.buffers, sizeof out->buffers);
    out->nconstants = one.nconstants;
    memcpy(out->constants, one.constants, sizeof out->constants);
    out->step_at = step_at;
    out->nsteps = nsteps;
    return NULL;
}
