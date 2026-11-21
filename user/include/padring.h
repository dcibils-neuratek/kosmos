/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_PADRING_H
#define KOSMOS_PADRING_H

#include <stdint.h>

#include "tcpring.h"            /* RING_BARRIER, and the argument for it */

/*
 * **The game controllers' latest state, in one page** (`docs/flight.md`,
 * F7). A pad reports hundreds of times a second, because the hardware says
 * so, so its state is a stream: it lives in a region and no report is a
 * message (*Control by message, data by shared memory*).
 *
 * Not a ring: what a reader wants is the state now, never the reports in
 * between, so each pad is one slot the driver writes over. init makes the
 * page and hands it to two processes - the USB driver, which may write it,
 * and the window manager, which may only read it - so nobody else can name
 * it, and the window manager hands the focused window its pads in the
 * frame's reply, as it hands it the pointer.
 *
 * **One writer and a sequence count per slot**: the driver makes `seq`
 * odd, writes the slot, and makes it even again. A reader keeps what it
 * read only when `seq` was even and the same before and after. A torn read
 * is two reports mixed, which for a stick is harmless - but it is refused
 * anyway, so a reader never has to think about it.
 *
 * `present` is 1 while a pad is plugged into that slot and 0 after it
 * goes; the slots are given out in the order pads arrive.
 */

#define PAD_RING_MAGIC   0x4450414bu        /* 'KAPD' */
#define PAD_RING_PADS    4u
#define PAD_RING_BYTES   4096u

struct pad_slot {
    uint32_t seq;               /* odd while the driver writes */
    uint32_t buttons;           /* `enum pad_bit` bits, `pad_decode.h` */
    int16_t  lx, ly, rx, ry;    /* -32768 to 32767, up and right positive */
    uint8_t  lt, rt;            /* the triggers, 0 to 255 */
    uint8_t  present;           /* 1 while a pad is in this slot */
    uint8_t  unused;
};

struct pad_ring {
    uint32_t magic;
    uint32_t pads;              /* PAD_RING_PADS */
    struct pad_slot slot[PAD_RING_PADS];
};

_Static_assert(sizeof(struct pad_slot) == 20, "a pad's slot is 20 bytes");
_Static_assert(sizeof(struct pad_ring) <= PAD_RING_BYTES, "the pads fit in a page");

/* Where slot `i` begins, for a reader that reads bytes (the window
 * manager, in Lua). */
#define PAD_RING_SLOT(i)  (8u + 20u * (i))

/* The driver's half: a slot written whole between two counts. */
static inline void pad_ring_write(volatile struct pad_ring *r, unsigned i,
                                  uint32_t buttons, int16_t lx, int16_t ly,
                                  int16_t rx, int16_t ry, uint8_t lt, uint8_t rt,
                                  uint8_t present)
{
    volatile struct pad_slot *s = &r->slot[i];
    uint32_t seq = s->seq;

    s->seq = seq | 1u;
    RING_BARRIER();
    s->buttons = buttons;
    s->lx = lx;
    s->ly = ly;
    s->rx = rx;
    s->ry = ry;
    s->lt = lt;
    s->rt = rt;
    s->present = present;
    RING_BARRIER();
    s->seq = (seq | 1u) + 1u;
}

#endif
