/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_NETPROTO_H
#define KOSMOS_NETPROTO_H

#include <stdint.h>

/*
 * /net: the stack, as a shape.
 *
 * One process holds the card - `SPAWN_NET`, which is the disk's grant
 * pointed outwards - and everything else asks it. That is what makes a
 * connection a capability later rather than a number anybody can name, and
 * it is why this is a protocol rather than a library.
 *
 * **Small, because ICMP is small.** Ethernet, ARP, IPv4 and echo is all
 * that is behind this today, and the operations are: what am I, ping that,
 * and what came back. TCP is a different piece of work and will bring
 * operations of its own; what it must not bring is data through here.
 *
 * ---
 *
 * **Where the shared ring goes, and why it is not here yet.**
 *
 * `CLAUDE.md` names a packet explicitly: if it recurs because the hardware
 * says so, the bytes live in a region and the message carries only where and
 * how much. That rule was learned from the audio server, which took a
 * 1024-byte period as a message payload 172 times a second and manufactured
 * 340 KB of garbage inside a 5.8 ms deadline.
 *
 * So it is worth being exact about where it bites here, rather than
 * discovering it the same way twice:
 *
 *   - **Card to driver** is already a shared ring. That is what a virtqueue
 *     is, and the device DMAs straight into the driver's buffers.
 *   - **Stack to application, for an echo**, is the one-shot case the rule
 *     allows. A ping happens because somebody asked, once, at whatever rate
 *     they typed - not because a clock came round. Sixty-four bytes in a
 *     message is a `getattr` reply with a different name.
 *   - **Stack to application, for a TCP stream**, is not. Bytes arrive
 *     because the far end sent them, at the wire's rate, for as long as the
 *     connection lasts. That is a period with a different clock, and it is
 *     exactly the shape that went wrong before.
 *
 * **So the ring is TCP's to add, and this header says so now** so that
 * whoever adds TCP finds the decision already made rather than finding that
 * a message worked for the first ten kilobytes. `audioring.h` is the model:
 * a single-producer, single-consumer ring with indices, nobody taking a
 * lock, and the message carrying only which slot is live.
 */

#define NET_OP_INFO      1u       /* what this stack is, and its addresses */
#define NET_OP_PING      2u       /* send one echo request */
#define NET_OP_REPLY     3u       /* collect whatever has come back */
#define NET_OP_CONFIG    4u       /* set the addresses */

#define NET_OK               0u
#define NET_ERR_BAD_OP       1u
#define NET_ERR_NO_CARD      2u   /* this machine has no network */
#define NET_ERR_NO_ROUTE     3u   /* nothing knows how to reach that address */
#define NET_ERR_UNREACHABLE  4u   /* nobody answered the ARP for it */
#define NET_ERR_FULL         5u   /* too many pings in flight at once */
#define NET_ERR_BAD_ADDRESS  6u

/*
 * How many echoes may be outstanding.
 *
 * A fixed pool, like every other server here: `ramfs` has its nodes and
 * `appfs` its table, and running out is an error at a known limit rather
 * than a failure at an unknown one. Eight is more than one person pinging
 * one host, which is what this is for.
 */
#define NET_PENDING_MAX     8u

/* The largest echo payload. A ping is conventionally 56 bytes of payload
 * and this is room for the ones that are not, without a message being
 * mostly padding. */
#define NET_PAYLOAD_MAX   128u

/*
 * An address, as four bytes.
 *
 * Not a `uint32_t`, and the difference matters more than it looks. An
 * address is four numbers in a written order, not a quantity - there is no
 * arithmetic anybody should do on it - and a 32-bit integer has a byte order
 * that this protocol would then have to name. Four bytes have the order they
 * are written in.
 *
 * The same argument `struct netinfo` makes about a MAC, one layer up.
 */
struct net_addr {
    uint8_t byte[4];
};

struct net_request {
    uint32_t op;
    uint32_t seq;                   /* the echo's sequence number */
    struct net_addr to;

    /* For NET_OP_CONFIG: this machine's address, its mask, and the router
     * to send anything outside the mask to. */
    struct net_addr address;
    struct net_addr netmask;
    struct net_addr gateway;

    uint32_t length;                /* payload bytes that follow */
    uint8_t  payload[NET_PAYLOAD_MAX];
};

/*
 * A reply, and the one field worth explaining.
 *
 * `ticks` is the physical counter, undecoded - not milliseconds. The same
 * division `hal_pointer_poll` draws by reporting device units: the counter's
 * frequency is `/dev/cpu`'s to report and the caller's to divide by, and a
 * server that converted here would be baking in a rate that is 62.5 MHz
 * under QEMU's TCG and 24 MHz when the same machine runs natively under
 * `hvf`. That is not a hypothetical - both happen on this machine, today.
 */
struct net_reply {
    uint32_t status;
    uint32_t seq;

    struct net_addr from;
    uint32_t ttl;
    uint64_t ticks;                 /* the round trip, in counter ticks */

    /* For NET_OP_INFO. */
    uint8_t  mac[6];
    uint16_t has_card;
    struct net_addr address;
    struct net_addr netmask;
    struct net_addr gateway;
    uint32_t mtu;

    uint32_t length;
    uint8_t  payload[NET_PAYLOAD_MAX];
};

#endif /* KOSMOS_NETPROTO_H */
