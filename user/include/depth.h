/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **How many periods of sound are kept queued** (`roadmap.md` 4i) - the one
 * rule the Synth Kit uses for its ring and the audio server for the device.
 *
 * A sound is heard after everything queued before it, so a queue kept full
 * is its whole length on every key: the kit's ring was 46 ms, the device's
 * 23. Kept shallow, a queue that runs dry is a click. So the depth is found
 * by playing, per machine, and from the safe side:
 *
 *   - **start full**;
 *   - **one fewer** for each window in which every look found at least two
 *     periods still queued - one spare after the one about to be taken;
 *   - **one more at once** when a look finds the queue empty, and that
 *     depth becomes a **floor** never probed below again, so a machine that
 *     cannot hold a depth runs dry there once and not again;
 *   - and, for a queue that asks it, **only while it is taken steadily**: a
 *     window counts toward coming down only if no look found more than
 *     `bite` periods gone since the last was put back.
 *
 * **Why the last.** One spare over the biggest bite a window saw is one
 * period of time only when the queue is taken a period at a time. QEMU's HD
 * Audio takes the device in bursts, and the audio server came down from four
 * to three on a window of two-period bites; the first bite of three emptied
 * it, and the film's sound had a gap in it - three runs in ten of
 * `x86-film`, none in seven with the device held at four (`testing.md`
 * 18.269). So the device asks for a bite of one and a window of a second: a
 * controller that takes a period at a time still comes down to two, and a
 * device that takes them in bursts stays where it is. The Synth Kit's ring
 * asks neither - a dry ring is one program's sound, not everybody's, and the
 * ring is most of a key's way to the ear.
 *
 * The kit's first version kept two and climbed on each dry run, which found
 * the same depth by running dry on the way at every start (`testing.md`
 * 18.262). The window was a second, and a note played in the first seconds
 * waited behind the whole ring; it is a quarter of one now (18.269).
 *
 * Header-only and freestanding, so the kit, the server and
 * `tools/test_depth.c` on the Mac compile the same lines. Times are the
 * counter's; the caller says how many counts a window is.
 */
#ifndef KOSMOS_DEPTH_H
#define KOSMOS_DEPTH_H

#include <stdbool.h>
#include <stdint.h>

struct depth {
    uint32_t kept;              /* periods kept queued now */
    uint32_t floor;             /* never fewer: the least allowed, or a dry's */
    uint32_t most;              /* the queue's size */
    uint32_t lowest;            /* the least a look found, this window */
    uint32_t dry;               /* looks that found it empty */
    uint32_t bite;              /* the most a look may find gone, or 0: any */
    uint64_t window_began;      /* the counter, when this window began */
    uint64_t changed_at;        /* the counter, when `kept` last moved */
};

static inline void depth_begin(struct depth *d, uint32_t most, uint32_t fewest,
                               uint32_t bite, uint64_t now)
{
    d->most = most > 0 ? most : 1;
    d->floor = fewest < 1 ? 1 : (fewest > d->most ? d->most : fewest);
    d->kept = d->most;
    d->lowest = UINT32_MAX;
    d->dry = 0;
    d->bite = bite;
    d->window_began = now;
    d->changed_at = now;
}

/*
 * **Looks begin again**, after a time without any - the device filled for a
 * new stream. A window starts now: one timed from before the pause would end
 * at the first look and be judged on that look alone. The audio server's
 * was timed from its start, so the first look of every stream could take a
 * period away on the evidence of one (`testing.md` 18.269).
 */
static inline void depth_resume(struct depth *d, uint64_t now)
{
    d->lowest = UINT32_MAX;
    d->window_began = now;
}

/* A look at the queue, `queued` periods in it, at the counter's `now`. */
static inline void depth_look(struct depth *d, uint32_t queued, uint64_t now,
                              uint64_t window)
{
    if (queued == 0) {
        d->dry++;

        if (d->kept < d->most) {
            d->kept++;
            d->changed_at = now;
        }

        if (d->floor < d->kept) {
            d->floor = d->kept;
        }

        d->lowest = UINT32_MAX;
        d->window_began = now;
        return;
    }

    if (queued < d->lowest) {
        d->lowest = queued;
    }

    if (now - d->window_began >= window) {
        bool spare = d->lowest >= 2;
        /* A queue can hold more than is kept, just after it came down. */
        bool steady = d->bite == 0 || d->lowest >= d->kept
                      || d->kept - d->lowest <= d->bite;

        if (spare && steady && d->kept > d->floor) {
            d->kept--;
            d->changed_at = now;
        }

        d->lowest = UINT32_MAX;
        d->window_began = now;
    }
}

#endif /* KOSMOS_DEPTH_H */
