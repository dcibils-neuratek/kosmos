/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **What a program's kernel may do on the GPU** (`docs/compute.md` C2,
 * decision D2): every one of a kernel's instructions decoded before the
 * driver keeps it, and the kernel refused - with the instruction and why -
 * when any of them could reach memory it was not handed, or leave the
 * kernel.
 *
 * The GPU has one table of memory, the global GTT, and the screens, every
 * window's picture and the rings are all in it. A kernel reaches memory only
 * by a `send` to a unit, so the rule is about sends: one to the data port
 * must name a binding-table entry - one of the buffers its run binds, each
 * bounded by the surface the driver made for it - and never a stateless,
 * shared-local or 64-bit address. Everything else is so a kernel cannot
 * escape the check: no compacted instructions (another encoding, not
 * decoded here), no indirect addressing, no register but the ones named
 * below, branches only to an instruction of the kernel, and the last
 * instruction the end of the thread.
 *
 * Generation 9's encoding, from Mesa's `pre_xe.json`; the data port's
 * message types from its `elk_eu_defines.h`. Pure: compiled into the
 * driver and on the Mac, where `tools/test_intelgpu_kernel.c` holds it to
 * kernels `eu.lua` assembled.
 */

#ifndef KOSMOS_INTELGPU_KERNEL_H
#define KOSMOS_INTELGPU_KERNEL_H

#include <stdint.h>

#define IGPU_KERNEL_MAX_BYTES   65536u   /* 4096 instructions */

struct igpu_kernel_verdict {
    uint32_t why;       /* GPU_KERNEL_*, 0 when it may run */
    uint32_t at;        /* the instruction refused, from 0 */
    int32_t  bti_max;   /* the highest binding-table entry a send names; -1 for none */
};

/* `bytes` of a kernel at `words`, checked. Answers `out->why`. */
uint32_t igpu_kernel_check(const uint32_t *words, uint32_t bytes,
                           struct igpu_kernel_verdict *out);

#endif
