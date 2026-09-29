/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **How many periods of sound the Synth Kit keeps queued** (`roadmap.md`
 * 4i). A sound is heard after everything queued before it, so a queue kept
 * full is its whole length on every key: the kit's ring was 46 ms. Kept
 * shallow, a queue that runs dry is a click. So the depth is found by
 * playing, per machine, and from the safe side:
 *
 *   - **start full**;
 *   - **one fewer** for each window in which every look found at least two
 *     periods still queued - one spare after the one about to be taken;
 *   - **one more at once** when a look finds the queue empty, and that
 *     depth becomes a **floor** never probed below again, so a machine that
 *     cannot hold a depth runs dry there once and not again.
 *
 * The kit's first version kept two and climbed on each dry run, which found
 * the same depth by running dry on the way at every start (`testing.md`
 * 18.262). The window was a second, and a note played in the first seconds
 * waited behind the whole ring; it is a quarter of one now (18.269).
 *
 * **The audio server does not keep the device by it**, and tried to. Finding
 * a floor this way costs a gap the first time the queue is taken faster than
 * a quiet window showed: on the kit's ring that is Groove's own sound, on the
 * device every program's - under QEMU's HD Audio two films in three, however
 * steady the evidence it asked for (18.276). The device stays at what its
 * driver holds.
 *
 * Header-only and freestanding, so the kit and `tools/test_depth.c` on the
 * Mac compile the same lines. Times are the
 * counter's; the caller says how many counts a window is.
 */
#ifndef KOSMOS_DEPTH_H
#define KOSMOS_DEPTH_H

#include <stdint.h>

struct depth {
    uint32_t kept;              /* periods kept queued now */
    uint32_t floor;             /* never fewer: the least allowed, or a dry's */
    uint32_t most;              /* the queue's size */
    uint32_t lowest;            /* the least a look found, this window */
    uint32_t dry;               /* looks that found it empty */
    uint64_t window_began;      /* the counter, when this window began */
    uint64_t changed_at;        /* the counter, when `kept` last moved */
};

static inline void depth_begin(struct depth *d, uint32_t most, uint32_t fewest,
                               uint64_t now)
{
    d->most = most > 0 ? most : 1;
    d->floor = fewest < 1 ? 1 : (fewest > d->most ? d->most : fewest);
    d->kept = d->most;
    d->lowest = UINT32_MAX;
    d->dry = 0;
    d->window_began = now;
    d->changed_at = now;
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
        if (d->lowest >= 2 && d->kept > d->floor) {
            d->kept--;
            d->changed_at = now;
        }

        d->lowest = UINT32_MAX;
        d->window_began = now;
    }
}

#endif /* KOSMOS_DEPTH_H */
