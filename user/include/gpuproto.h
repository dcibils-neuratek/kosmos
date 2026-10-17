/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_GPUPROTO_H
#define KOSMOS_GPUPROTO_H

#include <stdint.h>

/*
 * What you may ask the GPU's driver, at `/Devices/gpu` (`docs/m700-2d.md`).
 * A declared shape, as `backlightproto.h` is: fixed fields each way.
 *
 * **Its first verb is a test, on purpose.** Step 3 of the plan is the cursor
 * plane, and the first write any Kosmos code makes to the M700's display
 * engine is this one: a 64 by 64 picture shown by the cursor plane for some
 * seconds in the middle of the screen, read back, and taken away again,
 * everything as it was. What it says is what the next step - the window
 * manager's pointer handed to the plane - stands on.
 */

#define GPU_OP_LOOK          1u     /* say the display engine again */
#define GPU_OP_CURSOR_TEST   2u     /* `seconds` of a test cursor, then off */

#define GPU_OK               0u
#define GPU_ERR_NO_DEVICE    1u     /* no Intel GPU this driver knows */
#define GPU_ERR_BAD_OP       2u
#define GPU_ERR_NO_PIPE      3u     /* no pipe running to show it on */
#define GPU_ERR_NO_MEMORY    4u     /* no contiguous pages for the picture */
#define GPU_ERR_GTT_BUSY     5u     /* the GTT slots chosen were in use */
#define GPU_ERR_UNCHANGED    6u     /* written, and read back different */

struct gpu_request {
    uint32_t op;
    uint32_t seconds;               /* cursor test: how long it shows, 1 to 30 */
};

/*
 * The registers as read back, for the test: the cursor's control, base and
 * position, its share of the display buffer and its first watermark, and
 * the pipe's underrun flag before and after.
 */
struct gpu_reply {
    uint32_t error;
    uint32_t curcntr, curbase, curpos, buf_cfg, wm0;
    uint32_t underrun_before, underrun_after;
};

_Static_assert(sizeof(struct gpu_request) == 8,
               "the GPU request is two words, gpu.lua packs \"<I4I4\"");
_Static_assert(sizeof(struct gpu_reply) == 32,
               "the GPU reply is eight words, gpu.lua unpacks \"<I4I4I4I4I4I4I4I4\"");

#endif /* KOSMOS_GPUPROTO_H */
