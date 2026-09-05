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
 * **LISTEN arrived with the HTTP server**, which is the argument
 * `roadmap.md` said would settle it. It is the second half of the state
 * machine and a way to hand a caller a connection it did not ask for, and
 * both were cheaper than the note predicting them suggested.
 */
#define NET_OP_CONNECT   5u       /* open one, and get its rings */
#define NET_OP_PUSH      6u       /* there is something in `out` */
#define NET_OP_WAIT      7u       /* block until bytes arrive or it closes */
#define NET_OP_CLOSE     8u       /* this end is done sending */

/*
 * And the other direction, which `netproto.h` said was not here.
 *
 * **An HTTP server is what changed it.** `LISTEN` claims a port; `ACCEPT`
 * parks until somebody connects and comes back with the connection, its
 * rings, and who it is from. The connection that arrives is an ordinary one
 * from then on - the same handle, the same ring, the same `WAIT`.
 */
#define NET_OP_LISTEN    9u       /* answer on this port */
#define NET_OP_ACCEPT   10u       /* park until somebody arrives */

/*
 * Wait on several at once - the thing this system has wanted six times.
 *
 * Live queries wanted it, the stack's own loop wanted it, `telnet` wanted
 * it, the web server's log wanted it, and `httpd` reading a request wanted
 * it. Every one of them settled for a poll on a timer, which is a wait
 * wearing a worse hat: it costs a wake-up whether or not anything happened
 * and it adds half a period of latency to everything that did.
 *
 * **A server that handles one request at a time is the shape of not having
 * this.** With it, `httpd` runs a coroutine per connection and makes
 * progress on whichever one is ready - which is what a "multithreaded"
 * server is actually for, on a system that has no threads and does not want
 * a lock around its interpreter.
 *
 * `handle` carries a *bitmask* of connections rather than a list, because
 * `NET_CONN_MAX` fits in a word and a mask needs no length field. `port`
 * carries the listener to watch, plus one, so that zero means none.
 *
 * **Two masks, because "ready" is not one question.** `handle` is what the
 * caller wants to read from and `writing` is what it wants to write to, and
 * the answer for each is a different fact: bytes have arrived, or room has
 * appeared. One mask cannot say which, and the version that had one is worth
 * recording because it deadlocked rather than merely being imprecise.
 *
 * It reported a connection writable when there was space *and* something
 * still queued - a rule written for `wait`, where the question is "has
 * anything happened". A server sending a page larger than the ring fills it,
 * yields, and waits for room; if the far end then acknowledges the whole
 * ring before the next pass, there is space and nothing queued, so nothing
 * was reported - and nothing ever would be again, because both ends were
 * now waiting for the other. Eight requests for a 16 KB file hung, and the
 * server logged not one of them.
 *
 * With the interest declared, each side is a plain question with a plain
 * answer, and neither can spin: a caller only asks about writing when a
 * write has already failed for want of room.
 */
#define NET_OP_POLL     11u       /* wait until any of these has something */

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
 * Sixteen, and a fixed pool like everything else here: each carries a 36 KB
 * region, so the whole pool is 576 KB and running out is an error at a known
 * limit rather than a failure at an unknown one.
 *
 * **It was four, which was the number a client needed.** A server needs one
 * slot for the port it listens on and one per conversation in flight, and
 * four meant three at a time. Sixteen because it fits in the bitmask
 * `NET_OP_POLL` uses, which is the constraint that decides it - a
 * seventeenth would need a list where a word does now.
 */
#define NET_CONN_MAX    16u

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

    /* For NET_OP_POLL only: the connections to watch for *room to write*,
     * where `handle` is the ones to watch for bytes to read. */
    uint32_t writing;
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

    /*
     * For NET_OP_POLL: which of them are ready, as the same bitmask the
     * request used, and whether the listener has somebody waiting.
     */
    uint32_t ready;
    uint32_t arrived;

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
