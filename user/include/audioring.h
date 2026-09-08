/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_AUDIORING_H
#define KOSMOS_AUDIORING_H

#include <stddef.h>
#include <stdint.h>

/*
 * The barrier that makes an index mean the data behind it.
 *
 * A single-producer, single-consumer ring needs exactly two orderings: the
 * writes must land before the index that publishes them, and the index must
 * be read before the data behind it. AArch64's memory model reorders both,
 * so `dmb ish` is a real instruction there.
 *
 * **On x86-64 it is a compiler barrier and nothing else.** TSO does not
 * reorder stores with stores or loads with loads, so the processor already
 * guarantees what the ARM instruction has to ask for - but the *compiler*
 * still will, and an empty asm with a memory clobber is what stops it. The
 * cost is zero instructions and the correctness argument is the same one.
 */
#if defined(__x86_64__)
#define RING_BARRIER()  __asm__ volatile("" ::: "memory")
#else
#define RING_BARRIER()  __asm__ volatile("dmb ish" ::: "memory")
#endif

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

    /*
     * Written by the server, read by the client.
     *
     * `frames_played` counts the frames of *this stream* that have come out
     * of the speaker, and it is the one number here that needs no
     * conversion at all: a frame is the audio clock, so it means the same
     * thing on both sides of this region without anybody reading
     * `counter_hz` first.
     *
     * **The name carries the unit because the rule says it must.**
     * `CLAUDE.md`: a field that crosses a boundary is `wait_ticks`, never
     * `ticks`. This one crosses the widest boundary there is - two
     * processes reading the same page - and it was called `played` for an
     * afternoon, which is exactly the shape of the two timeout bugs that
     * rule was written after. A number that arrives naked gets read in
     * whatever unit the reader assumed.
     *
     * A position and a latency are both made of it. Where a stream has got
     * to is `frames_played`; how long until what is written now is heard
     * is `write * period_frames - frames_played`, which is the ring and
     * the device's
     * own queue together and does not have to know how the total is split
     * between them.
     *
     * Sixty-four bits because Kosmos is: an aligned 64-bit store is one
     * instruction on both architectures, so a client cannot read half of an
     * update. Thirty-two would have wrapped after twenty-seven hours at
     * 44100 - not a case anybody would reach, and therefore a special case
     * nobody would have tested either.
     *
     * It lands at 24 with no padding, because `read` ends there and 24 is
     * already eight-aligned. That is luck rather than design, and the
     * assertions below are what turn it into design: this struct is a
     * region two processes read, so where a field starts is part of the
     * agreement between them, and an agreement the compiler could silently
     * change is not one. A `uint32_t` inserted anywhere above moves
     * `frames_played` and fails the build rather than the sound.
     */
    volatile uint32_t read;
    volatile uint64_t frames_played;
};

/*
 * The layout, asserted rather than assumed.
 *
 * `audioproto.h` does the same for its messages and says why: the field
 * somebody will change without thinking is the one whose failure is silent.
 * A moved `frames_played` is worse than a moved message field - the server
 * writes
 * one offset and the client reads another, so the position reads as a
 * plausible number that is simply never right.
 */
_Static_assert(offsetof(struct audio_ring, write) == 16,
               "the client's index moved; both sides must be rebuilt");
_Static_assert(offsetof(struct audio_ring, read) == 20,
               "the server's index moved; both sides must be rebuilt");
_Static_assert(offsetof(struct audio_ring, frames_played) == 24,
               "the position moved, or needs padding it does not have");
_Static_assert(sizeof(struct audio_ring) <= AUDIO_RING_DATA,
               "the header has grown into the samples");

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
    RING_BARRIER();
    r->write = to;
}

static inline void audio_ring_consumed(struct audio_ring *r, uint32_t to)
{
    RING_BARRIER();
    r->read = to;
}

/* And the other direction: an index seen means the samples behind it are
 * there, so the read of the data must not be hoisted above the read of the
 * index that said it was ready. */
static inline uint32_t audio_ring_acquire(const volatile uint32_t *index)
{
    uint32_t v = *index;

    RING_BARRIER();
    return v;
}

/*
 * What has been heard, from what was taken and what is still held.
 *
 * Pure arithmetic, and separate from the server for exactly one reason:
 * `tools/test_audioring.c` can call it on this machine. It is the same
 * argument that keeps `kfs.lua` runnable on the host - the part that can be
 * wrong *silently* is the part that should not need an emulator to check,
 * and an off-by-one in a position reads as perfectly plausible audio.
 *
 * **Exact while a stream is feeding, and conservative after it starves.**
 * `held` is what the device still has, and this assumes all of it is this
 * stream. After a turn where the ring was empty the server mixed without
 * it, so some of what the device holds is somebody else's and this
 * subtracts too much: the position reads a little behind, never ahead, and
 * by at most the depth of the device.
 *
 * That is the right way round rather than a shrug. A progress bar that
 * pauses for a moment after an underrun is a glitch you already had; one
 * that jumps backwards is a bug report.
 */
static inline uint64_t audio_frames_played(uint64_t consumed, uint64_t held)
{
    return (consumed > held) ? consumed - held : 0;
}

/*
 * The position, published.
 *
 * **No barrier, deliberately.** `audio_ring_publish` needs one because the
 * index it writes is a promise about samples nobody may read early. This
 * promises nothing: there is no data behind it, it *is* the datum, and a
 * client that reads a stale one computes a slightly pessimistic latency and
 * then reads a fresh one a period later.
 *
 * `CLAUDE.md` asks for a barrier to be named and justified rather than
 * sprinkled. The honest justification here is that there is not one.
 */
static inline void audio_ring_position(struct audio_ring *r, uint64_t frames)
{
    r->frames_played = frames;
}

static inline uint64_t audio_ring_played(const struct audio_ring *r)
{
    return r->frames_played;
}

/*
 * How long between a frame written now and that frame being heard.
 *
 * Both terms are frames, so this is exact and involves no clock: `write` is
 * how many periods the client has published, `frames_played` how many have
 * been heard, and the difference is everything still in flight.
 *
 * Clamped rather than trusted. The two are written by different processes
 * and read here without a lock, so a client can pair a `write` with a
 * `frames_played` from just after it. The error is under one period; the
 * unclamped answer would be an enormous unsigned number, which is the
 * failure mode this system has already had twice with timeouts.
 */
static inline uint64_t audio_ring_delay(const struct audio_ring *r,
                                        uint32_t period_frames)
{
    uint64_t written = (uint64_t)r->write * period_frames;
    uint64_t heard = r->frames_played;

    return (written > heard) ? written - heard : 0;
}

static inline int audio_ring_valid(const struct audio_ring *r)
{
    return r != NULL && r->magic == AUDIO_RING_MAGIC && r->periods > 0
           && r->period_bytes > 0;
}

#endif /* KOSMOS_AUDIORING_H */
