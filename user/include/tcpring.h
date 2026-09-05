/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_TCPRING_H
#define KOSMOS_TCPRING_H

#include <stdint.h>

/*
 * A connection's bytes, in memory the stack and its client both hold.
 *
 * **`netproto.h` said this would be here and why.** A TCP stream is not the
 * one-shot case: bytes arrive because the far end sent them, at the wire's
 * rate, for as long as the connection lasts. That is a period with a
 * different clock, and `CLAUDE.md` names a packet explicitly. The audio
 * server is what the rule was learned from and the cost was 340 KB a second
 * of garbage inside a 5.8 ms deadline - so this is written before the first
 * byte moves rather than after somebody finds a message worked for the
 * first ten kilobytes.
 *
 * **Two rings, because a connection runs both ways**, and each one is
 * single-producer, single-consumer with the same discipline `audioring.h`
 * sets out: one side only writes and advances `write`, the other only reads
 * and advances `read`, neither index is touched by both, and so no lock is
 * needed rather than merely omitted.
 *
 *   `out` - the client writes, the stack sends.
 *   `in`  - the stack writes what arrived, the client reads.
 *
 * The indices are monotonic and unsigned, so wrapping at 2^32 is arithmetic
 * rather than a special case: `write - read` is right across the wrap, which
 * a pair of "current position" indices would not be.
 *
 * **What the ring is not is a window.** TCP's receive window is what this
 * end advertises to the far end, and it is `in`'s free space - so a client
 * that stops reading really does slow the sender down, which is what flow
 * control is. Getting that wrong in the other direction is a stack that
 * advertises room it does not have and drops what arrives, which looks like
 * a lossy network.
 */

#define TCP_RING_MAGIC   0x4b543152u        /* 'R1TK' */

/*
 * Sixteen kilobytes each way.
 *
 * Bigger than a window needs to be for a line protocol and small enough that
 * four connections are 128 KB. What decides it properly is a measurement of
 * throughput against window size, and there is nothing to measure yet;
 * `roadmap.md` M12 is where that argument belongs.
 */
#define TCP_RING_BYTES   16384u

/* The data starts a page in, so the indices never share a cache line with
 * the bytes. One core makes that free; two make it the difference between a
 * ring and a ping-pong of invalidations, and `CLAUDE.md` asks for SMP-ready
 * now. */
#define TCP_RING_DATA    4096u

struct tcp_ring {
    uint32_t magic;
    uint32_t bytes;                 /* capacity of each direction */
    uint32_t reserved[2];

    /* Written by the client, read by the stack. */
    volatile uint32_t out_write;
    volatile uint32_t out_read;     /* the stack advances this */

    /* Written by the stack, read by the client. */
    volatile uint32_t in_write;
    volatile uint32_t in_read;      /* the client advances this */

    /*
     * The connection is over, said by the side that noticed.
     *
     * A flag rather than a message, because the bytes already in the ring
     * have to be readable *after* it is set: a far end that sends a line and
     * closes has sent that line, and a client that saw the close first and
     * stopped reading would lose it. So this means "no more will arrive",
     * not "stop".
     */
    volatile uint32_t closed;
};

static inline uint32_t tcp_ring_ready(uint32_t write, uint32_t read)
{
    return write - read;
}

static inline uint32_t tcp_ring_space(uint32_t bytes, uint32_t write,
                                      uint32_t read)
{
    return bytes - (write - read);
}

static inline uint8_t *tcp_ring_out(struct tcp_ring *r)
{
    return (uint8_t *)r + TCP_RING_DATA;
}

static inline uint8_t *tcp_ring_in(struct tcp_ring *r)
{
    return (uint8_t *)r + TCP_RING_DATA + r->bytes;
}

/*
 * The bytes must be visible before the index that publishes them.
 *
 * `dmb ish` rather than `dsb`: this orders two normal-memory accesses
 * against each other for the other observers in the inner shareable domain,
 * which is what another core reading this ring is. `dsb` would additionally
 * wait for completion, and nothing here needs that.
 */
static inline void tcp_ring_publish(volatile uint32_t *index, uint32_t to)
{
    __asm__ volatile("dmb ish" ::: "memory");
    *index = to;
}

/* And the other direction: an index seen means the bytes behind it are
 * there, so reading the data must not be hoisted above reading the index
 * that said it was ready. */
static inline uint32_t tcp_ring_acquire(const volatile uint32_t *index)
{
    uint32_t v = *index;

    __asm__ volatile("dmb ish" ::: "memory");

    return v;
}

/* How big a region one connection needs. */
#define TCP_RING_REGION  (TCP_RING_DATA + 2u * TCP_RING_BYTES)

#endif /* KOSMOS_TCPRING_H */
