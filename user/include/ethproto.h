/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_ETHPROTO_H
#define KOSMOS_ETHPROTO_H

#include <stdint.h>

/*
 * What the network stack may ask the driver that holds the wire.
 *
 * A declared shape, like `audioproto.h` and for the reasons it gives: the
 * thing on the other side of this message is a system component, it has to
 * stay correct when the caller is wrong or out of date, and a struct means
 * most wrong shapes cannot be expressed rather than having to be checked.
 *
 * **Three operations and no more.** Attach, which hands over the ring and
 * asks what the adapter is; send, which says there are frames in the ring;
 * and info, which asks again - for the link, which changes without anybody
 * asking. Everything else about a frame is in the region (`ethring.h`).
 *
 * **`send` carries no frame and no count.** It says "look at the ring", and
 * the driver drains whatever is there. So a stack that has five frames to
 * put out writes five and calls once, and the number of messages is the
 * number of times the stack went idle rather than the number of frames.
 */

#define ETH_OP_ATTACH   1u      /* a ring capability travels with this one */
#define ETH_OP_SEND     2u      /* there are frames in the ring's `out` */
#define ETH_OP_INFO     3u      /* the adapter and its link, asked again */

/* Errors are numbers. The words belong to whoever shows them to a person. */
#define ETH_OK               0u
#define ETH_ERR_NO_ADAPTER   1u
#define ETH_ERR_NO_RING      2u
#define ETH_ERR_BAD_OP       3u
#define ETH_ERR_TAKEN        4u  /* an adapter answers one stack */

struct eth_request {
    uint32_t op;
    uint32_t reserved;
};

struct eth_reply {
    uint32_t error;             /* ETH_OK, or why not */
    uint32_t present;           /* whether there is an adapter at all */
    uint32_t mtu;               /* the largest frame it carries */
    uint32_t link;              /* 1 up, 0 down or never said */
    uint32_t sent;              /* frames the driver has put on the wire */
    uint32_t received;          /* ...and taken off it */
    uint8_t  mac[6];
    uint8_t  reserved[2];
};

_Static_assert(sizeof(struct eth_request) == 8, "eth_request is 8 bytes");
_Static_assert(sizeof(struct eth_reply) == 32, "eth_reply is 32 bytes");

#endif /* KOSMOS_ETHPROTO_H */
