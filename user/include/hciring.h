/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_HCIRING_H
#define KOSMOS_HCIRING_H

#include <stdint.h>

#include "tcpring.h"            /* RING_BARRIER, and the argument for it */

/*
 * **HCI packets between the Bluetooth host stack and its transport**
 * (`bluetooth.md` B2) - the USB driver, or `btsim` in its place.
 *
 * A region and not messages, by the rule `ethring.h` argues: a mouse's
 * report every 8 ms, or every millisecond for a gaming mouse, recurs
 * because the hardware says so. Two rings, single-producer and
 * single-consumer each, `ethring.h`'s discipline exactly:
 *
 *   `down` - the stack fills with commands and ACL data, the transport
 *            sends them to the controller;
 *   `up`   - the transport fills with events and ACL data that arrived,
 *            the stack reads them.
 *
 * **A slot is one packet**, its first byte the packet's type as HCI's
 * UART transport numbers them (Core Volume 4 Part A 2): 1 a command, 2 ACL
 * data, 4 an event. USB carries the type in which endpoint a packet goes
 * on rather than in a byte, so the USB driver reads and writes it here and
 * the stack never learns which transport it has - which is what lets the
 * Pi's radio, on a UART, carry the same stack.
 *
 * The indices are monotonic and unsigned: `write - read` is right across
 * the wrap.
 */

#define HCI_RING_MAGIC   0x4b494348u        /* 'HCIK' */

/* Packet types, the first byte of a slot (Volume 4 Part A 2). */
#define HCI_PACKET_COMMAND  1u
#define HCI_PACKET_ACL      2u
#define HCI_PACKET_EVENT    4u

/*
 * A slot holds the largest packet the stack will meet, with its type: an
 * event is 2 + 255 bytes, a command 3 + 255, and ACL data 4 + the
 * controller's buffer, which is 1021 for Classic's largest packet and 251
 * for Low Energy's. 2048 is the round number above all of them.
 */
#define HCI_RING_SLOT    2048u

/* Thirty-two each way: a keyboard's and a mouse's reports, a scan's
 * results, and room for the stack to be busy for a while without the
 * transport dropping one. */
#define HCI_RING_SLOTS   32u

/* The packets start a page in, so the indices never share a cache line
 * with them. */
#define HCI_RING_DATA    4096u

struct hci_ring {
    uint32_t magic;
    uint32_t slots;                 /* HCI_RING_SLOTS, as the stack built it */
    uint32_t slot_bytes;            /* HCI_RING_SLOT */
    uint32_t reserved;

    /* Written by the stack, read by the transport. */
    volatile uint32_t down_write;
    volatile uint32_t down_read;    /* the transport advances this */

    /* Written by the transport, read by the stack. */
    volatile uint32_t up_write;
    volatile uint32_t up_read;      /* the stack advances this */

    /* Packets the transport had no slot for: the stack fell behind. */
    volatile uint32_t up_dropped;
    uint32_t reserved2[3];

    /* How long each slot's packet is, its type byte included, published
     * with its index. */
    volatile uint32_t down_length[HCI_RING_SLOTS];
    volatile uint32_t up_length[HCI_RING_SLOTS];
};

#define HCI_RING_REGION  (HCI_RING_DATA + 2u * HCI_RING_SLOTS * HCI_RING_SLOT)

_Static_assert(sizeof(struct hci_ring) <= HCI_RING_DATA,
               "the indices fit before the packets");

static inline uint32_t hci_ring_ready(uint32_t write, uint32_t read)
{
    return write - read;
}

static inline uint32_t hci_ring_space(uint32_t write, uint32_t read)
{
    return HCI_RING_SLOTS - (write - read);
}

static inline uint8_t *hci_ring_down(struct hci_ring *r, uint32_t index)
{
    return (uint8_t *)r + HCI_RING_DATA
           + (index % HCI_RING_SLOTS) * HCI_RING_SLOT;
}

static inline uint8_t *hci_ring_up(struct hci_ring *r, uint32_t index)
{
    return (uint8_t *)r + HCI_RING_DATA
           + (HCI_RING_SLOTS + index % HCI_RING_SLOTS) * HCI_RING_SLOT;
}

/* The packet and its length visible before the index that publishes them;
 * `tcpring.h` has the argument for both orderings. */
static inline void hci_ring_publish(volatile uint32_t *index, uint32_t to)
{
    RING_BARRIER();
    *index = to;
}

static inline uint32_t hci_ring_acquire(const volatile uint32_t *index)
{
    uint32_t v = *index;

    RING_BARRIER();

    return v;
}

/* Whether a region handed over is one of these at all: a transport maps
 * what a caller sends it, and refuses a region of another shape rather
 * than reading lengths out of somebody else's bytes. */
static inline int hci_ring_valid(const struct hci_ring *r)
{
    return r != 0 && r->magic == HCI_RING_MAGIC
           && r->slots == HCI_RING_SLOTS && r->slot_bytes == HCI_RING_SLOT;
}

/* A length read from the ring is the other side's to choose: one outside a
 * slot, or too short to hold a type byte, is not a packet. */
static inline int hci_ring_length_ok(uint32_t length)
{
    return length >= 1u && length <= HCI_RING_SLOT;
}

#endif /* KOSMOS_HCIRING_H */
