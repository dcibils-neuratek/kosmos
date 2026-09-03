/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_AUDIORING_H
#define KOSMOS_AUDIORING_H

#include <stdint.h>

/*
 * A stream of samples between two processes, in memory they both hold.
 *
 * **This exists because `CLAUDE.md` says a stream never travels as a message
 * payload**, and audio was the subsystem that had not learned it. A period
 * used to be sent to the audio server as message bytes: a Lua string minted
 * on the client, copied into a message, copied out, and minted again on the
 * server - 340 KB a second of garbage manufactured inside a 5.8 ms deadline,
 * with the collector arriving whenever it liked. It played, and it jittered,
 * and no amount of scheduling was ever going to fix it because the GC was in
 * the audio path by construction.
 *
 * The window manager has always done the other thing: an application draws
 * into a shared surface and the message says which buffer is live. This is
 * that, for sound.
 *
 * **Single producer, single consumer, and no lock.** The client only ever
 * writes samples and advances `write`; the server only ever reads them and
 * advances `read`. Neither index is touched by both sides, which is what
 * makes a lock unnecessary rather than merely omitted - and it is the
 * discipline `CLAUDE.md` attaches to the rule, because a region that two
 * processes both write is a region that needs something this does not have.
 *
 * The indices are monotonic and unsigned, so wrapping at 2^32 is arithmetic
 * rather than a special case: `write - read` is right across the wrap, which
 * a pair of "current slot" indices would not be.
 */

#define AUDIO_RING_MAGIC   0x4453314bu      /* 'K1SD' */

/*
 * Eight periods, 46 ms.
 *
 * The ring is how far *ahead* a client may get, and it is not latency on its
 * own - what reaches the speaker is whatever the device holds. It is slack:
 * room for a client to be late once without the device noticing, which is
 * exactly what was missing. Eight is a starting value and `audiolag` is how
 * it gets argued about.
 */
#define AUDIO_RING_PERIODS 8u

/*
 * Samples start a page in.
 *
 * Not for alignment - the region is page-aligned anyway - but so that the
 * two indices never share a cache line with the samples. One core today
 * makes that free; two cores make it the difference between a ring and a
 * ping-pong of invalidations, and `CLAUDE.md` asks for SMP-ready now.
 */
#define AUDIO_RING_DATA    4096u

struct audio_ring {
    uint32_t magic;
    uint32_t periods;
    uint32_t period_bytes;
    uint32_t reserved;

    /* Written by the client, read by the server. */
    volatile uint32_t write;

    /* Written by the server, read by the client. */
    volatile uint32_t read;
};

static inline uint32_t audio_ring_ready(const struct audio_ring *r)
{
    return r->write - r->read;
}

static inline uint32_t audio_ring_space(const struct audio_ring *r)
{
    return r->periods - audio_ring_ready(r);
}

static inline uint8_t *audio_ring_slot(struct audio_ring *r, uint32_t index)
{
    return (uint8_t *)r + AUDIO_RING_DATA
           + (size_t)(index % r->periods) * r->period_bytes;
}

/*
 * The samples must be visible before the index that publishes them.
 *
 * `dmb ish` rather than `dsb`: this orders two normal-memory accesses
 * against each other for the other observers in the inner shareable domain,
 * which is what another core reading this ring is. `dsb` would additionally
 * wait for completion, which nothing here needs - and `CLAUDE.md` asks for
 * the barrier to be named and justified rather than sprinkled.
 */
static inline void audio_ring_publish(struct audio_ring *r, uint32_t to)
{
    __asm__ volatile("dmb ish" ::: "memory");
    r->write = to;
}

static inline void audio_ring_consumed(struct audio_ring *r, uint32_t to)
{
    __asm__ volatile("dmb ish" ::: "memory");
    r->read = to;
}

/* And the other direction: an index seen means the samples behind it are
 * there, so the read of the data must not be hoisted above the read of the
 * index that said it was ready. */
static inline uint32_t audio_ring_acquire(const volatile uint32_t *index)
{
    uint32_t v = *index;

    __asm__ volatile("dmb ish" ::: "memory");
    return v;
}

static inline int audio_ring_valid(const struct audio_ring *r)
{
    return r != NULL && r->magic == AUDIO_RING_MAGIC && r->periods > 0
           && r->period_bytes > 0;
}

#endif /* KOSMOS_AUDIORING_H */
