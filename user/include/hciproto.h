/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_HCIPROTO_H
#define KOSMOS_HCIPROTO_H

#include <stdint.h>

/*
 * **What the Bluetooth host stack may ask its transport** (`bluetooth.md`
 * B2), and what the transport tells it unasked. A declared shape, as
 * `ethproto.h` is and for its reasons.
 *
 * **Three requests.** Attach hands over the ring (`hciring.h`) and asks
 * whether there is an adapter; send says there are packets in `down`;
 * about asks which adapter is there, after a pulse says one arrived. The
 * packets themselves are only ever in the ring.
 *
 * **And three pulses**, to the stack's own endpoint, which init gives the
 * transport: an adapter arrived, there is something in `up`, the adapter
 * left. A pulse never waits (`pulses.md`), so the transport is never held
 * by a stack that is busy, and the stack hears all three on the receive
 * where it hears everything else. An adapter may come and go many times
 * under one attach: the ring is the channel between two processes, not
 * a device's.
 */

#define HCI_OP_ATTACH   1u      /* a ring capability travels with this one */
#define HCI_OP_SEND     2u      /* there are packets in the ring's `down` */
#define HCI_OP_ABOUT    3u      /* which adapter is there now, if any */

/* Pulse codes, to the stack (`kosmos_pulse`). The value is the adapter's
 * generation: a number the transport gives each adapter as it arrives, so
 * a "more" from one that has since left is told apart. */
#define HCI_PULSE_ADAPTER  1u   /* an adapter is ready: Reset it */
#define HCI_PULSE_MORE     2u   /* there are packets in `up` */
#define HCI_PULSE_GONE     3u   /* the adapter left; its packets with it */

/* Errors are numbers. The words belong to whoever shows them to a person. */
#define HCI_OK               0u
#define HCI_ERR_NO_ADAPTER   1u
#define HCI_ERR_NO_RING      2u
#define HCI_ERR_BAD_OP       3u
#define HCI_ERR_TAKEN        4u  /* a transport answers one stack */

/* What carries the adapter, for the stack's line in the log. */
#define HCI_BUS_USB          1u
#define HCI_BUS_SIMULATED    2u  /* `btsim`: a controller in software */

struct hci_request {
    uint32_t op;
    uint32_t reserved;
};

struct hci_reply {
    uint32_t error;             /* HCI_OK, or why not */
    uint32_t present;           /* whether there is an adapter now */
    uint32_t generation;        /* its number, as the pulses carry it */
    uint16_t bus;               /* HCI_BUS_* */
    uint16_t port;              /* the USB port, or 0 */
    uint16_t vendor;            /* the USB vendor and product, or 0 */
    uint16_t product;
};

_Static_assert(sizeof(struct hci_request) == 8, "hci_request is 8 bytes");
_Static_assert(sizeof(struct hci_reply) == 20, "hci_reply is 20 bytes");

#endif /* KOSMOS_HCIPROTO_H */
