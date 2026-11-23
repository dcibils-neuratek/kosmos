/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_SERIESRING_H
#define KOSMOS_SERIESRING_H

#include <stddef.h>
#include <stdint.h>

/*
 * **A history of numbers, as one block of memory with a declared shape**
 * (`docs/sysmonitor.md`, the Series Kit, step S1).
 *
 * A monitor keeps a few dozen series - a core's kernel and user time, a
 * drive's reading and writing, the GPU's clock - each a number a pace, and
 * draws the last minute, ten minutes or hour of them. `ui.history` kept
 * them as a Lua table a sample, which is a hundred thousand small tables
 * for the collector to walk once an hour is kept. These are the same
 * numbers in a flat block that only C reads.
 *
 * **The shape is declared because it is meant to cross a boundary later.**
 * Today the ring lives in the window that keeps it, in its own memory, and
 * nothing crosses. Decision 2's recorder (S10) would keep the same rings
 * from the machine's start in a region the Deskbar and System Monitor map
 * read-only; this header is what both would compile against, so the ring
 * a window keeps today is the ring a region holds then, unchanged.
 *
 * **Three steps, each a ring of columns** (decision 6):
 *
 *   step 0  a column a push - the pace, 0.5, 1 or 2 s - 600 of them
 *   step 1  a column every 10 s, 360 of them: an hour
 *   step 2  a column every 60 s, 1,440 of them: a day
 *
 * A column holds, for every series, **the mean and the highest** of the
 * samples that fell in it - at the pace the two are the sample itself - so
 * a spike of one second still shows in an hour's graph. The counts are
 * fields rather than constants: a ring says how many it holds, and the
 * defaults below are what a monitor asks for.
 *
 * **A missing sample is a gap, not a zero.** A source that did not answer
 * this pace is NaN in both halves of its pair, and a coarser column that
 * no sample reached is NaN too. A zero would draw as an idle machine; a gap
 * draws as nothing, which is what was known.
 *
 * **Doubles, not floats.** `sysmonitor.md` sized the pairs as `float`, 19
 * KB a series. A float holds a byte count exactly only to 16 MB, and a
 * disk's or a network's rate is past that; the mirrored graphs print their
 * scale, and `ui.history.draw` must draw what it drew from the Lua numbers
 * it was handed, pixel for pixel. So a pair is two doubles, 38 KB a series
 * at the default counts, about 3 MB for the eighty the M700 would keep.
 *
 * **One producer, any number of readers, and no lock.** Only the producer
 * writes a column, and it publishes each step's `written` with a release
 * store after the column is complete. A reader loads `written` with an
 * acquire, copies the column, and loads it again: if the producer has come
 * round to that column's slot meanwhile, the copy may be torn and is
 * refused (`series_ring_copy`). `written` is the only field both touch,
 * and a step keeps a slot spare so that the oldest column it holds is
 * never the one the producer is writing.
 *
 * The memory, in order:
 *
 *   struct series_ring                    the header, below
 *   struct series_label   [series]        each series' name and unit
 *   then, for each step, a slot more than its columns:
 *     uint64_t            at   [columns + 1]  the counter when it began
 *     struct series_pair  pair [columns + 1][series]
 */

#define SERIES_RING_MAGIC    0x31524553u   /* 'SER1' */
#define SERIES_STEPS         3u

/* What a monitor asks for: a pace's ten minutes at 1 s, an hour at 10 s,
 * a day at 60 s. */
#define SERIES_PACE_COLUMNS  600u
#define SERIES_TEN_COLUMNS   360u
#define SERIES_SIXTY_COLUMNS 1440u

#define SERIES_NAME_BYTES    24u
#define SERIES_UNIT_BYTES    8u

/*
 * **The most series one ring holds.** Not a limit on a monitor, which keeps
 * as many rings as it likes: a bound on one block, so a producer and a
 * reader agree on its size by arithmetic that cannot overflow. Eighty is
 * the M700's whole machine; one ring a page holds far fewer.
 */
#define SERIES_MAX           256u

struct series_step {
    uint32_t columns;       /* how many this step holds; 0: the step is off */
    uint32_t reserved;

    /*
     * A column's width in counter ticks - 10 s and 60 s of the counter -
     * and 0 at the pace, where a column is a push. Counter ticks and not
     * seconds because the column's `at` is the counter, and a number that
     * crosses a boundary says which clock it is (`CLAUDE.md`, two clocks).
     */
    uint64_t width_counter_ticks;

    /*
     * Columns ever written, published with a release store. **A step has
     * one slot more than it holds**: the slot of column `i` is `i % (columns
     * + 1)`, and the spare is the one the producer writes next, so the
     * oldest column a reader is told is held is never the one being
     * written over.
     */
    uint64_t written;

    /* Where this step's `at` array starts, in bytes from the ring's start;
     * its pairs follow it. */
    uint64_t offset;
};

struct series_ring {
    uint32_t magic;
    uint32_t series;                    /* how many series, 1 to SERIES_MAX */
    uint64_t counter_hz;                /* 0: nothing here is a time */
    uint64_t pace_counter_ticks;        /* what the producer pushes at; 0 unsaid */
    uint64_t bytes;                     /* the whole block */
    struct series_step step[SERIES_STEPS];
};

struct series_label {
    char name[SERIES_NAME_BYTES];       /* "kernel", "read"; NUL-padded */
    char unit[SERIES_UNIT_BYTES];       /* "%", "B/s" */
};

struct series_pair {
    double mean;                        /* NaN: a gap */
    double high;
};

/*
 * The layout, asserted rather than assumed, as `audioring.h` does and for
 * its reason: where a field starts is part of the agreement between two
 * processes, and the compiler is not a party to it.
 */
_Static_assert(sizeof(struct series_step) == 32, "a step's header moved");
_Static_assert(offsetof(struct series_ring, step) == 32, "the steps moved");
_Static_assert(sizeof(struct series_ring) == 32 + 32 * SERIES_STEPS,
               "the ring's header changed size; both sides must be rebuilt");
_Static_assert(sizeof(struct series_label) == 32, "a label changed size");
_Static_assert(sizeof(struct series_pair) == 16, "a pair changed size");

/* Up to a multiple of 64, so each step's data starts on a cache line. */
static inline uint64_t series_ring_round(uint64_t n)
{
    return (n + 63u) & ~(uint64_t)63u;
}

/* The slots a step of `columns` has: one spare, and none for a step off. */
static inline uint64_t series_ring_slots(uint32_t columns)
{
    return columns == 0 ? 0 : (uint64_t)columns + 1;
}

/* The bytes one step of `columns` takes for `series` series. */
static inline uint64_t series_ring_step_bytes(uint32_t series, uint32_t columns)
{
    uint64_t slots = series_ring_slots(columns);

    return series_ring_round(slots * sizeof(uint64_t)
                             + slots * series * sizeof(struct series_pair));
}

/*
 * The whole block for `series` series and the three steps' `columns`, or 0
 * for a shape no ring may have. Every count is bounded, so the sum is.
 */
static inline uint64_t series_ring_bytes(uint32_t series, const uint32_t columns[SERIES_STEPS])
{
    uint64_t n;
    unsigned k;

    if (series == 0 || series > SERIES_MAX) {
        return 0;
    }

    n = series_ring_round(sizeof(struct series_ring)
                          + (uint64_t)series * sizeof(struct series_label));

    for (k = 0; k < SERIES_STEPS; k++) {
        if (columns[k] > (1u << 20)) {
            return 0;
        }
        n += series_ring_step_bytes(series, columns[k]);
    }

    return n;
}

static inline struct series_label *series_ring_labels(struct series_ring *r)
{
    return (struct series_label *)(void *)(r + 1);
}

/* The `at` of slot `slot` of step `k`. */
static inline uint64_t *series_ring_at(struct series_ring *r, unsigned k, uint32_t slot)
{
    return (uint64_t *)(void *)((uint8_t *)r + r->step[k].offset) + slot;
}

/* The `series` pairs of slot `slot` of step `k`. */
static inline struct series_pair *series_ring_pairs(struct series_ring *r, unsigned k,
                                                    uint32_t slot)
{
    uint8_t *base = (uint8_t *)r + r->step[k].offset
                    + series_ring_slots(r->step[k].columns) * sizeof(uint64_t);

    return (struct series_pair *)(void *)base + (uint64_t)slot * r->series;
}

static inline const struct series_pair *
series_ring_pairs_c(const struct series_ring *r, unsigned k, uint32_t slot)
{
    return series_ring_pairs((struct series_ring *)(uintptr_t)r, k, slot);
}

/*
 * **The reader's half.** `written` with an acquire: a column counted is a
 * column whose bytes are there.
 */
static inline uint64_t series_ring_written(const struct series_ring *r, unsigned k)
{
    return __atomic_load_n(&r->step[k].written, __ATOMIC_ACQUIRE);
}

/*
 * Column `index` of step `k` - counted from the first ever written - into
 * `out` (`series` pairs) and `*at`. 0 when it is not held: not written yet,
 * or written over. The second load of `written` is what refuses a column
 * the producer came round to while it was being copied: the column that
 * shares this slot is `index + columns + 1`, and the producer starts on it
 * only after it has published `written == index + columns + 1`, so a count
 * no higher than `index + columns` after the copy means nothing touched it.
 */
static inline int series_ring_copy(const struct series_ring *r, unsigned k, uint64_t index,
                                   struct series_pair *out, uint64_t *at)
{
    uint32_t columns = r->step[k].columns;
    uint64_t written = series_ring_written(r, k);
    uint32_t slot, i;
    const struct series_pair *p;

    if (columns == 0 || index >= written || written - index > columns) {
        return 0;
    }

    slot = (uint32_t)(index % series_ring_slots(columns));
    p = series_ring_pairs_c(r, k, slot);

    for (i = 0; i < r->series; i++) {
        out[i] = p[i];
    }

    if (at != NULL) {
        *at = *series_ring_at((struct series_ring *)(uintptr_t)r, k, slot);
    }

    /* The copy's loads, before the second look at the count. */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    return __atomic_load_n(&r->step[k].written, __ATOMIC_RELAXED) - index <= columns;
}

/*
 * **The producer's half.** Before a slot is written over, the count that
 * said it was free must be visible first - a release fence orders the
 * earlier store of `written` before the slot's - and after it is written,
 * the new count is published with a release store. On x86-64 both are
 * compiler barriers; on AArch64 they are `dmb ish`, the barrier
 * `audioring.h` names and for the same reason.
 */
static inline void series_ring_begin(void)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

static inline void series_ring_publish(struct series_ring *r, unsigned k, uint64_t written)
{
    __atomic_store_n(&r->step[k].written, written, __ATOMIC_RELEASE);
}

#endif /* KOSMOS_SERIESRING_H */
