/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **An emulator of the GPU's execution units** (`docs/cafesa3d-gpu.md` R2,
 * decision E3): a kernel `eu.lua` assembled, run on the Mac as the M700's
 * HD 530 would run it - a thread for each group of the grid, eight or
 * sixteen lanes, the GRF, the flags, the structured branches by each
 * lane's own instruction pointer as the PRM writes them (PcIP), and the
 * data port's messages into buffers bounded as the hardware bounds a
 * surface. So every kernel - the fill, GPU Fractal's, the lesson's, the
 * tracer's - is tested in `make test`, where there is no GPU, and held to
 * the processor's answer.
 *
 * The subset is `eu.lua`'s: what it does not assemble, this does not run,
 * and says so rather than guessing. Floating point is the processor's,
 * single precision, `mad` fused; the GPU's own `math` is approximate where
 * libm is exact, so a kernel's answers are held within a tolerance, never
 * bit for bit, where `math` is in them.
 */

#ifndef KOSMOS_EU_EMU_H
#define KOSMOS_EU_EMU_H

#include <stddef.h>
#include <stdint.h>

/* A run's buffer, as the driver makes its surface (`intelgpu.c`, `run_surface`). */
struct eu_buffer {
    uint8_t *bytes;
    uint32_t width, height, pitch;  /* as imported: four-byte pixels at pitch */
    uint32_t view;                  /* GPU_VIEW_* */
};

struct eu_run {
    const uint32_t *kernel;
    uint32_t bytes;
    uint32_t simd;                  /* 8 or 16 */
    uint32_t groups_x, groups_y;
    struct eu_buffer *buffers;
    uint32_t nbuffers;
    const uint8_t *constants;       /* r1 onwards */
    uint32_t nconstants;
    uint64_t most_steps;            /* a thread's instructions before it is called stuck; 0 for a million */
};

struct eu_stats {
    uint64_t threads, instructions, lanes;  /* lanes: an instruction's enabled lanes, summed */
};

/* Each instruction's runs counted into `per_ip`, by its index, from the
 * next eu_emulate on; NULL stops it. For a profile (`test_k3d_gpu.c`). */
void eu_profile(uint64_t *per_ip);

/* Runs every thread of the grid. NULL, or why it could not - with the
 * instruction it stopped at in `*at` when there is one. */
const char *eu_emulate(const struct eu_run *run, struct eu_stats *stats, uint32_t *at);

#endif
