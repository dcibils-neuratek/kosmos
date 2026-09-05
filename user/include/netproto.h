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
 * **Two halves, and the line between them is where the bytes go.**
 * `INFO`, `CONFIG` and `PING` carry everything they need in the message,
 * because each is one question asked once. `CONNECT` and the operations
 * after it carry no data at all: a connection's bytes live in a region both
 * sides hold - `tcpring.h` - and the messages say only that something
 * happened.
 *
 * ---
 *
 * **Where the shared ring goes.**
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
 * **That decision was written here before TCP existed**, so that whoever
 * added it would find it made rather than find that a message worked for the
 * first ten kilobytes. `tcpring.h` is what came of it, on `audioring.h`'s
 * model: single-producer, single-consumer, indices, nobody taking a lock.
 */

#define NET_OP_INFO      1u       /* what this stack is, and its addresses */
#define NET_OP_PING      2u       /* send one echo request */
#define NET_OP_REPLY     3u       /* collect whatever has come back */
#define NET_OP_CONFIG    4u       /* set the addresses */

/*
 * A connection, and the four things a client does with one.
 *
 * `CONNECT` hands back a *capability to a region* - the two rings in
 * `tcpring.h` - and a handle to name it by. After that the bytes never
 * travel in a message: the client writes into `out` and says `PUSH`, the
 * stack writes into `in` and the client reads it. `WAIT` is how a client
 * blocks until something happens rather than asking in a loop.
 *
 * **No LISTEN**, and that is a scope decision rather than an omission. A
 * client connects out, which is what telnet and SSH do and what this stack
 * is for today; accepting a connection needs a second half of the state
 * machine and a way to hand a caller a connection it did not ask for.
 * `roadmap.md`'s HTTP server is where that argument belongs.
 */
#define NET_OP_CONNECT   5u       /* open one, and get its rings */
#define NET_OP_PUSH      6u       /* there is something in `out` */
#define NET_OP_WAIT      7u       /* block until bytes arrive or it closes */
#define NET_OP_CLOSE     8u       /* this end is done sending */

#define NET_OK               0u
#define NET_ERR_BAD_OP       1u
#define NET_ERR_NO_CARD      2u   /* this machine has no network */
#define NET_ERR_NO_ROUTE     3u   /* nothing knows how to reach that address */
#define NET_ERR_UNREACHABLE  4u   /* nobody answered the ARP for it */
#define NET_ERR_FULL         5u   /* too many pings in flight at once */
#define NET_ERR_BAD_ADDRESS  6u
#define NET_ERR_REFUSED      7u   /* the far end said no */
#define NET_ERR_CLOSED       8u   /* the connection is over */
#define NET_ERR_TIMEOUT      9u   /* nobody answered in time */
#define NET_ERR_NO_HANDLE   10u   /* no such connection */

/*
 * How many connections at once.
 *
 * Four, and a fixed pool like everything else here: each carries a 36 KB
 * region, and running out is an error at a known limit rather than a
 * failure at an unknown one. Four is a telnet, an SSH and room to be wrong
 * about how many somebody wants.
 */
#define NET_CONN_MAX     4u

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
    uint32_t handle;                /* which connection, for the TCP ops */
    uint32_t port;                  /* where to connect */
    uint32_t ticks;                 /* how long NET_OP_WAIT may wait */
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
    uint32_t handle;                /* the connection this is about */
    uint32_t ring_bytes;            /* capacity of each direction */
    uint32_t state;                 /* NET_TCP_*, for a connection */

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

/*
 * What a connection is doing, as the one thing a client needs to know.
 *
 * Not TCP's state machine - that is the stack's business and has nine
 * states, most of which are about closing tidily. A client wants to know
 * whether it may write, and these are the three answers.
 */
#define NET_TCP_OPENING  1u
#define NET_TCP_OPEN     2u
#define NET_TCP_CLOSED   3u

#endif /* KOSMOS_NETPROTO_H */
