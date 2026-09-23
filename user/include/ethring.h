/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_ETHRING_H
#define KOSMOS_ETHRING_H

#include <stdint.h>

#include "tcpring.h"            /* RING_BARRIER, and the argument for it */

/*
 * Frames between the network stack and the driver that holds the wire.
 *
 * **Why this is a region and not a message.** `CLAUDE.md`: control by
 * message, data by shared memory, and the test is the rate - if it recurs
 * because the hardware says so, the bytes live in a region. A frame is the
 * canonical case, named in that rule beside a period and a block. The audio
 * server is what the rule was learned from, and the cost of getting it wrong
 * there was 340 KB a second of garbage inside a 5.8 ms deadline.
 *
 * **Why it exists at all**, which is the interesting half. Until now the
 * stack called `kosmos_net_send` and `kosmos_net_recv`, which are *syscalls*
 * into the kernel's own virtio driver. A USB Ethernet adapter is driven by
 * a process (`xhci.c`), and the kernel must not learn what USB is - so the
 * frames have to cross from one EL0 process to another, and this is the
 * crossing (`roadmap.md` 5m-d, `usb.md` 7d).
 *
 * **Two rings, single-producer and single-consumer each**, exactly the
 * discipline `tcpring.h` sets out: one side only writes and advances
 * `write`, the other only reads and advances `read`, neither index is
 * touched by both, and no lock is needed rather than merely omitted.
 *
 *   `out` - the stack fills, the driver puts on the wire.
 *   `in`  - the driver fills with what arrived, the stack reads.
 *
 * **Slots rather than bytes**, which is the one place this differs from a
 * TCP ring. A frame has a length and a boundary; a stream does not. So a
 * slot holds one frame and an array beside the indices says how long each
 * one is - written before the index that publishes it, like the bytes
 * themselves.
 *
 * The indices are monotonic and unsigned, so wrapping at 2^32 is arithmetic
 * rather than a special case: `write - read` is right across the wrap.
 */

#define ETH_RING_MAGIC   0x4b544845u        /* 'ETHK' */

/*
 * A slot holds any frame an Ethernet adapter will carry: the largest an ECM
 * device claims is 1514, and 2048 is the round number above it with room for
 * a VLAN tag and whatever the next specification adds.
 */
#define ETH_RING_SLOT    2048u

/*
 * Thirty-two each way. At a gigabit and 1500 bytes that is about four tenths
 * of a millisecond of buffering, which is enough to cover the stack being
 * busy for one pass and not enough to hide a stack that has stopped reading.
 * What decides it properly is a measurement, and there is nothing to measure
 * until frames move.
 */
#define ETH_RING_SLOTS   32u

/* The frames start a page in, so the indices never share a cache line with
 * them - the same reason `tcpring.h` does it. */
#define ETH_RING_DATA    4096u

struct eth_ring {
    uint32_t magic;
    uint32_t slots;                 /* ETH_RING_SLOTS, as the maker built it */
    uint32_t slot_bytes;            /* ETH_RING_SLOT */
    uint32_t reserved;

    /* Written by the stack, read by the driver. */
    volatile uint32_t out_write;
    volatile uint32_t out_read;     /* the driver advances this */

    /* Written by the driver, read by the stack. */
    volatile uint32_t in_write;
    volatile uint32_t in_read;      /* the stack advances this */

    /* How long the frame in each slot is, published with its index. */
    volatile uint32_t out_length[ETH_RING_SLOTS];
    volatile uint32_t in_length[ETH_RING_SLOTS];
};

#define ETH_RING_REGION  (ETH_RING_DATA + 2u * ETH_RING_SLOTS * ETH_RING_SLOT)

static inline uint32_t eth_ring_ready(uint32_t write, uint32_t read)
{
    return write - read;
}

static inline uint32_t eth_ring_space(uint32_t write, uint32_t read)
{
    return ETH_RING_SLOTS - (write - read);
}

static inline uint8_t *eth_ring_out(struct eth_ring *r, uint32_t index)
{
    return (uint8_t *)r + ETH_RING_DATA
           + (index % ETH_RING_SLOTS) * ETH_RING_SLOT;
}

static inline uint8_t *eth_ring_in(struct eth_ring *r, uint32_t index)
{
    return (uint8_t *)r + ETH_RING_DATA
           + (ETH_RING_SLOTS + index % ETH_RING_SLOTS) * ETH_RING_SLOT;
}

/* The frame and its length must be visible before the index that publishes
 * them; an index seen means what is behind it is there. Both orderings, and
 * the argument for each, are `tcpring.h`'s. */
static inline void eth_ring_publish(volatile uint32_t *index, uint32_t to)
{
    RING_BARRIER();
    *index = to;
}

static inline uint32_t eth_ring_acquire(const volatile uint32_t *index)
{
    uint32_t v = *index;

    RING_BARRIER();

    return v;
}

/*
 * Whether a region handed over is one of these at all.
 *
 * The driver maps what a caller sends it, so this is the same check
 * `audio_ring_valid` is: a client that sends a region of the wrong shape
 * gets a refusal rather than the driver reading a length out of somebody
 * else's bytes.
 */
static inline int eth_ring_valid(const struct eth_ring *r)
{
    return r != 0 && r->magic == ETH_RING_MAGIC
           && r->slots == ETH_RING_SLOTS && r->slot_bytes == ETH_RING_SLOT;
}

#endif /* KOSMOS_ETHRING_H */
