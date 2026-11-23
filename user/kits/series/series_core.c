/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Series Kit's core** (`series_core.h`, `docs/sysmonitor.md` S1):
 * numbers kept in rings at three steps, and drawn as the columns
 * `ui.history.draw` drew.
 *
 * **Why C.** A history is a loop over columns on every sample and every
 * repaint, and data the collector should never walk: `ui.history` was a
 * Lua table a sample and a `g:fill` from Lua a column - sixty a graph for a
 * minute, six hundred for ten. Here a sample is a few doubles written into
 * a ring, and a graph is one call that hands its rectangles to gfx.
 *
 * **Not vector code, on purpose.** `CLAUDE.md` asks for the vector unit
 * where a loop over pixels or samples can use it. Neither loop here is one:
 * a column is one rectangle a series, filled by gfx's own row loop, which
 * is already vectorised (`rows.c`); and a coarser column's mean and
 * highest are a sum and a comparison over a sample's handful of series,
 * once a pace. `sysmonitor.md` says so, and the host test holds the plain
 * loop.
 *
 * **The drawing is today's, exactly.** Every number below is the one
 * `ui.history.draw` computed in Lua - its floor divisions on integers, its
 * `math.floor(... + 0.5)` on doubles - so Monitor, Disk Monitor, Network
 * Monitor, GPU Monitor and LLM Monitor draw through this unchanged, and
 * `tools/test_series.c` holds it to that Lua run by Lua.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "seriesring.h"
#include "series_core.h"

/* One coarser step's open column: which one it is, and what fell in it. */
struct open_column {
    bool     open;          /* a sample has arrived for `bucket` */
    bool     started;       /* anything has, so `next` means something */
    uint64_t bucket;        /* the open column, as now / width */
    uint64_t next;          /* the bucket the next column written stands for */
};

struct series {
    struct series_ring *ring;
    uint32_t            n;
    struct open_column  col[SERIES_STEPS];

    /* [SERIES_STEPS][n] each: the open column's sums, highest, counts. */
    double             *sum;
    double             *high;
    uint32_t           *count;
};

static double gap(void)
{
    return __builtin_nan("");
}

static bool is_gap(double v)
{
    return v != v;
}

static size_t round64(size_t n)
{
    return (n + 63u) & ~(size_t)63u;
}

/* The producer's half, before the ring. */
static size_t keeper_bytes(uint32_t n)
{
    return round64(sizeof(struct series))
           + round64((size_t)SERIES_STEPS * n * (2 * sizeof(double) + sizeof(uint32_t)));
}

size_t series_size(const struct series_config *c)
{
    uint64_t ring;

    if (c == NULL) {
        return 0;
    }

    ring = series_ring_bytes(c->series, c->columns);

    if (ring == 0 || c->columns[0] == 0) {
        return 0;
    }

    return keeper_bytes(c->series) + (size_t)ring;
}

struct series *series_make(void *memory, const struct series_config *c)
{
    size_t bytes = series_size(c);
    struct series *s = memory;
    struct series_ring *r;
    uint8_t *at;
    uint64_t offset;
    unsigned k;

    if (bytes == 0 || memory == NULL || ((uintptr_t)memory & 7u) != 0) {
        return NULL;
    }

    memset(memory, 0, bytes);

    at = (uint8_t *)memory + round64(sizeof(struct series));
    s->n = c->series;
    s->sum = (double *)(void *)at;
    s->high = s->sum + (size_t)SERIES_STEPS * s->n;
    s->count = (uint32_t *)(void *)(s->high + (size_t)SERIES_STEPS * s->n);
    s->ring = r = (struct series_ring *)(void *)((uint8_t *)memory + keeper_bytes(s->n));

    r->magic = SERIES_RING_MAGIC;
    r->series = c->series;
    r->counter_hz = c->counter_hz;
    r->pace_counter_ticks = c->pace_counter_ticks;
    r->bytes = series_ring_bytes(c->series, c->columns);

    offset = series_ring_round(sizeof(struct series_ring)
                               + (uint64_t)c->series * sizeof(struct series_label));

    for (k = 0; k < SERIES_STEPS; k++) {
        struct series_step *st = &r->step[k];

        /* A coarser step without a counter to measure ten seconds by is
         * off: the door's rings, which keep only the pace. */
        st->columns = (k == 0 || c->counter_hz != 0) ? c->columns[k] : 0;
        st->width_counter_ticks = k == 0 ? 0
                                  : c->counter_hz * (k == 1 ? SERIES_TEN_SECONDS
                                                            : SERIES_SIXTY_SECONDS);
        st->offset = offset;
        offset += series_ring_step_bytes(c->series, c->columns[k]);
    }

    return s;
}

struct series_ring *series_ring_of(struct series *s)
{
    return s->ring;
}

const struct series_ring *series_ring_of_c(const struct series *s)
{
    return s->ring;
}

void series_clear(struct series *s)
{
    unsigned k;

    for (k = 0; k < SERIES_STEPS; k++) {
        series_ring_publish(s->ring, k, 0);
        memset(&s->col[k], 0, sizeof s->col[k]);
    }
}

/* ---------------------------------------------------------- the producer */

/*
 * A column into step `k`: its `at`, and a mean and a highest a series -
 * NULL for both is a gap. Written into the slot, then published.
 */
static void write_column(struct series *s, unsigned k, uint64_t at,
                         const double *mean, const double *high)
{
    struct series_ring *r = s->ring;
    uint64_t written = __atomic_load_n(&r->step[k].written, __ATOMIC_RELAXED);
    uint32_t slot = (uint32_t)(written % series_ring_slots(r->step[k].columns));
    struct series_pair *p = series_ring_pairs(r, k, slot);
    uint32_t i;

    series_ring_begin();

    *series_ring_at(r, k, slot) = at;

    for (i = 0; i < s->n; i++) {
        p[i].mean = mean != NULL ? mean[i] : gap();
        p[i].high = high != NULL ? high[i] : gap();
    }

    series_ring_publish(r, k, written + 1);
}

/* The open column of step `k` written: each series' mean and highest, or a
 * gap for a series no sample in it answered. */
static void close_column(struct series *s, unsigned k)
{
    struct open_column *c = &s->col[k];
    double *sum = s->sum + (size_t)k * s->n;
    double *high = s->high + (size_t)k * s->n;
    uint32_t *count = s->count + (size_t)k * s->n;
    uint32_t i;

    for (i = 0; i < s->n; i++) {
        if (count[i] == 0) {
            sum[i] = high[i] = gap();
        } else {
            sum[i] = sum[i] / (double)count[i];
        }
    }

    write_column(s, k, c->bucket * s->ring->step[k].width_counter_ticks, sum, high);
    c->open = false;
    c->next = c->bucket + 1;
}

/* Gaps for every column from `next` to before `bucket` - only as many as
 * the ring holds, since any more would be written over at once. */
static void gaps_until(struct series *s, unsigned k, uint64_t bucket)
{
    struct open_column *c = &s->col[k];
    uint64_t columns = s->ring->step[k].columns;
    uint64_t from = c->next;

    if (bucket <= from) {
        return;
    }

    if (bucket - from > columns) {
        from = bucket - columns;
    }

    for (; from < bucket; from++) {
        write_column(s, k, from * s->ring->step[k].width_counter_ticks, NULL, NULL);
    }

    c->next = bucket;
}

static void open_column(struct series *s, unsigned k, uint64_t bucket)
{
    struct open_column *c = &s->col[k];
    uint32_t i;

    c->open = true;
    c->started = true;
    c->bucket = bucket;

    for (i = 0; i < s->n; i++) {
        s->sum[(size_t)k * s->n + i] = 0.0;
        s->high[(size_t)k * s->n + i] = gap();
        s->count[(size_t)k * s->n + i] = 0;
    }
}

static void accumulate(struct series *s, unsigned k, const double *values)
{
    double *sum = s->sum + (size_t)k * s->n;
    double *high = s->high + (size_t)k * s->n;
    uint32_t *count = s->count + (size_t)k * s->n;
    uint32_t i;

    for (i = 0; i < s->n; i++) {
        double v = values[i];

        if (is_gap(v)) {
            continue;
        }

        sum[i] += v;
        count[i]++;

        if (is_gap(high[i]) || v > high[i]) {
            high[i] = v;
        }
    }
}

/* Step `k` brought to `bucket`: the open column closed if it is over, the
 * columns since it as gaps. */
static void advance(struct series *s, unsigned k, uint64_t bucket)
{
    struct open_column *c = &s->col[k];

    if (c->open && bucket > c->bucket) {
        close_column(s, k);
    }

    if (c->started && !c->open) {
        gaps_until(s, k, bucket);
    }
}

void series_push(struct series *s, const double *values, uint64_t now)
{
    unsigned k;

    write_column(s, 0, now, values, values);

    for (k = 1; k < SERIES_STEPS; k++) {
        uint64_t width = s->ring->step[k].width_counter_ticks;
        struct open_column *c = &s->col[k];
        uint64_t bucket;

        if (s->ring->step[k].columns == 0 || width == 0) {
            continue;
        }

        bucket = now / width;

        /* A counter that went back - a sample stamped out of order - is
         * counted in the open column rather than reopening a closed one. */
        if (c->open && bucket < c->bucket) {
            bucket = c->bucket;
        }

        advance(s, k, bucket);

        if (!c->open) {
            if (c->started && bucket < c->next) {
                bucket = c->next;
            }
            open_column(s, k, bucket);
        }

        accumulate(s, k, values);
    }
}

void series_tick(struct series *s, uint64_t now)
{
    unsigned k;

    for (k = 1; k < SERIES_STEPS; k++) {
        uint64_t width = s->ring->step[k].width_counter_ticks;

        if (s->ring->step[k].columns == 0 || width == 0) {
            continue;
        }

        advance(s, k, now / width);
    }
}

/* ------------------------------------------------------------ the reader */

double series_most(const struct series *s, unsigned k, uint64_t upto, uint64_t count)
{
    struct series_pair pairs[SERIES_MAX];
    double most = 0;
    uint64_t i;

    if (k >= SERIES_STEPS) {
        return 0;
    }

    for (i = 0; i < count; i++) {
        uint32_t j;

        if (upto + i < count
            || !series_ring_copy(s->ring, k, upto - count + i, pairs, NULL)) {
            continue;
        }

        for (j = 0; j < s->n; j++) {
            if (is_gap(pairs[j].high)) {
                break;
            }
            if (pairs[j].high > most) {
                most = pairs[j].high;
            }
        }
    }

    return most;
}

/* ------------------------------------------------------------ the drawing */

/*
 * **Lua's `//` on integers**: the floor, where C's `/` truncates. A column
 * left of the box - a history longer than its `columns` - has a negative
 * slot, and the two differ exactly there.
 */
static long floor_div(long a, long b)
{
    long q = a / b;

    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        q--;
    }

    return q;
}

/*
 * **`math.floor` of a double**, as a long. Without libm - the door is
 * freestanding - and clamped at 2^52, past which every rectangle is off
 * every surface anyway and a long would overflow where Lua's float did not.
 */
static long floor_d(double d)
{
    const double big = 4503599627370496.0;
    long t;

    if (d != d) {
        return 0;
    }
    if (d >= big) {
        return (long)big;
    }
    if (d <= -big) {
        return -(long)big;
    }

    t = (long)d;

    if ((double)t > d) {
        t--;
    }

    return t;
}

/* `paint.scale`'s `px`: points to pixels, to the nearest. */
static long px(long v, double pct)
{
    return floor_d((double)v * pct / 100.0 + 0.5);
}

/*
 * One rectangle, as `gc:fill` and then `paint.lua` would have drawn it:
 * clipped to the view in force, a rectangle wholly outside it not drawn at
 * all, then scaled by its two edges so neighbours still meet.
 */
static void emit(const struct series_place *p, series_fill_fn fill, void *ctx,
                 long x, long y, long w, long h, uint32_t colour)
{
    long x0 = x, y0 = y, x1 = x + w, y1 = y + h;

    if (p != NULL) {
        if (x0 < p->cx) { x0 = p->cx; }
        if (y0 < p->cy) { y0 = p->cy; }
        if (x1 > p->cx + p->cw) { x1 = p->cx + p->cw; }
        if (y1 > p->cy + p->ch) { y1 = p->cy + p->ch; }
    }

    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    if (p != NULL && p->pct > 0 && p->pct != 100) {
        long sx = px(x0, p->pct), sy = px(y0, p->pct);

        fill(ctx, sx, sy, px(x1, p->pct) - sx, px(y1, p->pct) - sy, colour);
        return;
    }

    fill(ctx, x0, y0, x1 - x0, y1 - y0, colour);
}

static uint32_t colour_of(const struct series_look *look, uint32_t k)
{
    /* A colour not given was `nil` to `g:fill`, and `paint.lua` paints a
     * fill with no colour opaque black. */
    return k < look->ncolours ? look->colours[k] : 0xff000000u;
}

void series_draw(const struct series *s, long x, long y, long w, long h,
                 const struct series_look *look, const struct series_place *place,
                 series_fill_fn fill, void *ctx)
{
    struct series_pair pairs[SERIES_MAX];
    long inner = h - 4;
    long base = y + h - 2;
    long half = floor_div(inner, 2);
    long centre = y + 2 + half;
    long prev_row[SERIES_MAX];
    bool prev_seen[SERIES_MAX];
    uint64_t i;
    uint32_t k;

    if (look->columns <= 0 || look->step >= SERIES_STEPS || fill == NULL) {
        return;
    }

    /* `if most <= 0 then return most end`: a scale of nothing draws none. */
    if (look->mode != SERIES_STACKED && !(look->most > 0)) {
        return;
    }

    for (k = 0; k < s->n; k++) {
        prev_seen[k] = false;
    }

    for (i = 0; i < look->count; i++) {
        long slot = look->columns - (long)look->count + (long)i;
        long x0 = x + 2 + floor_div(slot * (w - 4), look->columns);
        long x1 = x + 2 + floor_div((slot + 1) * (w - 4), look->columns);
        long cw = x1 - x0 - 1 > 1 ? x1 - x0 - 1 : 1;

        if (look->upto + i < look->count
            || !series_ring_copy(s->ring, look->step, look->upto - look->count + i,
                                 pairs, NULL)) {
            for (k = 0; k < s->n; k++) {
                prev_seen[k] = false;
            }
            continue;
        }

        if (look->mode == SERIES_MIRRORED) {
            /* `s[1] or 0`: a gap is nothing either way. */
            double in = s->n > 0 && !is_gap(pairs[0].mean) ? pairs[0].mean : 0;
            double out = s->n > 1 && !is_gap(pairs[1].mean) ? pairs[1].mean : 0;
            long up = floor_d((double)half * in / look->most + 0.5);
            long down = floor_d((double)half * out / look->most + 0.5);

            if (up > half) { up = half; }
            if (down > half) { down = half; }

            if (up > 0) {
                emit(place, fill, ctx, x0, centre - up, cw, up, colour_of(look, 0));
            }
            if (down > 0) {
                emit(place, fill, ctx, x0, centre + 1, cw, down, colour_of(look, 1));
            }
        } else if (look->mode == SERIES_LINE) {
            /*
             * **A line a series**, new in S1: a point a column at its share
             * of `most`, a pixel tall across the column, and a pixel wide
             * from the last column's point to this one's, so a series reads
             * as one stepped line. A gap breaks it.
             */
            for (k = 0; k < s->n; k++) {
                double v = pairs[k].mean;
                long level, row;

                if (is_gap(v) || inner <= 0) {
                    prev_seen[k] = false;
                    continue;
                }

                level = floor_d((double)(inner - 1) * v / look->most + 0.5);
                if (level < 0) { level = 0; }
                if (level > inner - 1) { level = inner - 1; }
                row = base - 1 - level;

                emit(place, fill, ctx, x0, row, cw, 1, colour_of(look, k));

                if (prev_seen[k] && prev_row[k] != row) {
                    long top = prev_row[k] < row ? prev_row[k] : row;
                    long bottom = prev_row[k] < row ? row : prev_row[k];

                    emit(place, fill, ctx, x0, top, 1, bottom - top + 1, colour_of(look, k));
                }

                prev_row[k] = row;
                prev_seen[k] = true;
            }
        } else {
            /* Stacked: each series on the one below it, up from the floor,
             * as far as the first gap - `ipairs` stopped at the first nil. */
            long at = base;

            for (k = 0; k < s->n; k++) {
                double v = pairs[k].mean;
                long part;

                if (is_gap(v)) {
                    break;
                }

                part = floor_d((double)inner * v / 100.0);

                if (part > 0) {
                    emit(place, fill, ctx, x0, at - part, cw, part, colour_of(look, k));
                    at = at - part;
                }
            }
        }
    }
}
