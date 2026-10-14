/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_VMMSCREEN_H
#define KOSMOS_VMMSCREEN_H

/*
 * **A sandbox's screen, as a region** (`docs/linux.md` L2): what the guest
 * draws through virtio-gpu, ready to be drawn. Whoever starts the sandbox
 * makes the region - this page, then the pixels - fills in `magic`,
 * `width` and `height`, and hands it to the monitor; the monitor lays each
 * rectangle the guest flushes into the pixels and counts it in `frames`.
 *
 * **Ready pixels**: Kosmos's own, 32 bits each - blue, green, red, alpha in
 * memory, alpha opaque - and `width * 4` bytes a row, which is a surface's
 * pitch because `width` is a multiple of 16. So a window wraps the pixels
 * as a surface (`gfx.wrap`) and blits them with nothing converted in Lua
 * or anywhere after this (`CLAUDE.md`, no pixel offset computed in Lua).
 *
 * One producer, one reader, and only counters both touch: the monitor
 * writes the pixels and then `frames`; a window reads `frames` and draws
 * when it moved (*control by message, data by shared memory*). A frame
 * drawn while the guest is mid-flush can tear; double buffering is a step
 * for when it is seen to.
 */

#include <stdint.h>

#define VMM_SCREEN_MAGIC    0x7263734bu     /* "Kscr" */
#define VMM_SCREEN_HEAD     4096u           /* the pixels start a page in */
#define VMM_SCREEN_MOST     4096u           /* the widest or tallest a screen is */

struct vmm_screen {
    uint32_t magic;
    uint32_t width;                 /* a multiple of 16 */
    uint32_t height;
    uint32_t pitch;                 /* bytes a row: `width * 4` */

    /* The monitor's, after the pixels of each flush are in. */
    volatile uint32_t frames;       /* flushes laid in, counted */
    volatile uint32_t shown;        /* 1 once the guest showed a picture */
    uint32_t x, y, w, h;            /* the last flush's rectangle */
};

_Static_assert(sizeof(struct vmm_screen) == 40, "ten words");

#endif
