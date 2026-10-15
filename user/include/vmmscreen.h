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

    /*
     * **A new size asked for** (`docs/linux.md` L5): the window writes
     * `width`, `height` and `pitch`, then counts it in `sized`; the monitor,
     * seeing the count move, tells the guest its display changed. The
     * region is made for the largest - `most_width` by `most_height`, the
     * desktop's - so a size is never more room than there is.
     */
    uint32_t most_width, most_height;
    volatile uint32_t sized;
    uint32_t reserved;
};

_Static_assert(sizeof(struct vmm_screen) == 56, "fourteen words");

/*
 * **The keyboard and the pointer, the other way** (`docs/linux.md` L3): a
 * ring in the same first page, from `VMM_INPUT_AT` - the window writes
 * events, the monitor reads them into the guest's two virtio-input
 * devices. Linux's own `input_event`s, in the guest's codes: a key's
 * number is Kosmos's own raw key's, which is the same numbering; the
 * pointer is absolute, `0` to `VMM_INPUT_ABS_MOST` across the picture.
 *
 * A single-producer, single-consumer ring of the shape `sys.ring_push`
 * writes (`sys_user.c`): `head` the window's, `tail` the monitor's, each
 * stored after what it covers, nothing else shared.
 */
#define VMM_INPUT_AT        1024u
#define VMM_INPUT_EVENTS    128u
#define VMM_INPUT_DEVICES   2u
#define VMM_INPUT_KEYBOARD  0u
#define VMM_INPUT_TABLET    1u
#define VMM_INPUT_ABS_MOST  32767u

struct vmm_input_event {
    uint16_t device;                /* VMM_INPUT_KEYBOARD or _TABLET */
    uint16_t type, code;            /* EV_KEY, EV_ABS, EV_REL, EV_SYN */
    uint16_t reserved;
    uint32_t value;
    uint32_t reserved2;
};

struct vmm_input_ring {
    volatile uint32_t head;         /* the window's: events written */
    volatile uint32_t tail;         /* the monitor's: events taken */
    uint32_t count;                 /* VMM_INPUT_EVENTS */
    uint32_t item;                  /* sizeof(struct vmm_input_event) */
    struct vmm_input_event ev[VMM_INPUT_EVENTS];
};

_Static_assert(sizeof(struct vmm_input_event) == 16, "four words an event");
_Static_assert(VMM_INPUT_AT + sizeof(struct vmm_input_ring) <= 4096u, "in the first page");

#endif
