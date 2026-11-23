/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_SERIES_CORE_H
#define KOSMOS_SERIES_CORE_H

/*
 * **The Series Kit's core: rings of numbers kept, and drawn as columns**
 * (`docs/sysmonitor.md`, step S1). No Lua and no surface here - the door
 * (`series_kosmos.c`) takes those - so `tools/test_series.c` holds this on
 * the Mac exactly as it runs in Kosmos.
 *
 * A series keeper is two things in one block of memory: the producer's own
 * state - each coarser step's open column, its sums and its highest so far
 * - and, after it, the ring (`seriesring.h`), which is the part a reader
 * sees. Kept apart so that the ring can one day be a region and the
 * producer's half stay private to the recorder.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "seriesring.h"

struct series_config {
    uint32_t series;                    /* 1 to SERIES_MAX */
    uint32_t columns[SERIES_STEPS];     /* each step's; 0 leaves it off */
    uint64_t counter_hz;                /* 0: no coarser steps, whatever columns says */
    uint64_t pace_counter_ticks;        /* written into the ring for its readers */
};

/* The coarser steps' widths, in seconds of the counter. */
#define SERIES_TEN_SECONDS    10u
#define SERIES_SIXTY_SECONDS  60u

struct series;

/* The bytes `series_make` wants for this shape, or 0 for one it refuses. */
size_t series_size(const struct series_config *c);

/* A keeper over `memory` (`series_size` bytes, 8-aligned), empty. */
struct series *series_make(void *memory, const struct series_config *c);

/* The ring a reader reads. */
struct series_ring *series_ring_of(struct series *s);
const struct series_ring *series_ring_of_c(const struct series *s);

/* Every step emptied; the shape and the labels kept. */
void series_clear(struct series *s);

/*
 * **One sample**: `values[series]`, NaN for a series with no answer this
 * time, at `now` on the counter. A column at the pace, and into each
 * coarser step's open column; a coarser column is written when a sample
 * arrives past its end, and every column between that no sample reached
 * is written as a gap.
 */
void series_push(struct series *s, const double *values, uint64_t now);

/*
 * The coarser steps brought up to `now` with no sample: an open column
 * whose time has passed is written, and the columns since as gaps. For a
 * keeper whose source stopped answering, so its hour does not stand still
 * at the last column that was heard.
 */
void series_tick(struct series *s, uint64_t now);

/*
 * The largest of the newest `count` columns of step `k` before column
 * `upto`, over every series - the highest of each pair - stopping in a
 * column at its first gap, as `ui.history.most` stops at a sample's first
 * nil. 0 when nothing is above it.
 */
double series_most(const struct series *s, unsigned k, uint64_t upto, uint64_t count);

/* --------------------------------------------------------------- drawing */

enum series_mode {
    SERIES_STACKED,     /* percentages, the first series at the floor */
    SERIES_MIRRORED,    /* two series about a centre line, one scale */
    SERIES_LINE,        /* each series a line, to `most` (100 unsaid) */
};

struct series_look {
    enum series_mode mode;
    unsigned  step;             /* which step's columns: 0, the pace */
    long      columns;          /* how many fill the width */
    uint64_t  count;            /* how many of the newest are drawn */
    uint64_t  upto;             /* drawn up to this column; the ring's `written` */
    double    most;             /* the mirrored and line scale */
    const uint32_t *colours;    /* one a series; past the end, opaque black */
    unsigned  ncolours;
};

/*
 * **Where the columns land**: the clip in force, in the coordinates the
 * columns are in, and the scale the window is drawn at. This is `ui.lua`'s
 * drawing context and `paint.lua`'s `scale` for a `fill`, done here in
 * one place so a column comes out of C exactly where the Lua `fill` it
 * replaces would have put it: clipped first, then scaled by its two edges.
 * A `pct` of 0 or 100 is no scale.
 */
struct series_place {
    long   cx, cy, cw, ch;
    double pct;
};

typedef void (*series_fill_fn)(void *ctx, long x, long y, long w, long h, uint32_t colour);

/*
 * **The columns of a history**, as `ui.history.draw` drew them, into the
 * `x`, `y`, `w`, `h` box: the newest at the right edge, each a
 * `columns`th of the width less the two-pixel inset, with a pixel of
 * ground between two. The ground, and the mirrored centre line, are the
 * caller's: they are one rectangle each, drawn the way every other one is.
 */
void series_draw(const struct series *s, long x, long y, long w, long h,
                 const struct series_look *look, const struct series_place *place,
                 series_fill_fn fill, void *ctx);

#endif /* KOSMOS_SERIES_CORE_H */
