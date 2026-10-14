/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A zlib stream deflated a piece at a time and flushed whole
 * (`user/kits/compress/zsync.c`), held on the Mac.
 *
 * Pieces of every size from a few bytes to a quarter of a megabyte, noisy
 * enough that many deflate to more than one 16 KB buffer - the case that
 * lost a band's tail on the M700 - each onto one stream, and after each the
 * stream so far inflated (with miniz's own inflater) and held to every byte
 * given so far. **The control**: the same pieces through the old stopping
 * rule, the first call that gave less than a buffer, which must lose bytes -
 * so this test is known to see what it is for.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"
#include "zsync.h"

static uint32_t seed = 1;

static uint32_t rnd(void)
{
    seed = seed * 1103515245u + 12345u;
    return seed >> 8;
}

/*
 * A piece of the kinds a screen's bands deflate to: noise, three-byte
 * pixels of a drifting colour, and those with RLE's run lengths between -
 * sizes from 17 KB to 217 KB, most deflating to more than one buffer.
 */
static size_t fill(uint8_t *p, size_t cap)
{
    size_t n = 17000 + rnd() % 200000, i = 0;
    unsigned mode = rnd() % 3, noise = 1 + rnd() % 60;
    unsigned r = rnd(), g = rnd(), b = rnd();

    if (n > cap) n = cap;

    while (i < n) {
        if (mode == 0) {
            p[i++] = (uint8_t)rnd();
            continue;
        }

        r += rnd() % noise;
        g += rnd() % 3;
        b += rnd() % 7;
        p[i++] = (uint8_t)r;
        if (i < n) p[i++] = (uint8_t)g;
        if (i < n) p[i++] = (uint8_t)b;
        if (mode == 2 && i < n) p[i++] = (uint8_t)(rnd() % 6 == 0 ? 255 : rnd() % 20);
    }

    return n;
}

struct sink { uint8_t *at; size_t n, cap; };

static void collect(void *ctx, const uint8_t *out, size_t n)
{
    struct sink *s = ctx;

    if (s->n + n > s->cap) {
        s->cap = (s->n + n) * 2;
        s->at = realloc(s->at, s->cap);
    }

    memcpy(s->at + s->n, out, n);
    s->n += n;
}

/* The rule before 14 October 2026: stop at the first short buffer. */
static void old_rule(tdefl_compressor *d, const uint8_t *in, size_t left, struct sink *s)
{
    static uint8_t chunk[16384];

    for (;;) {
        size_t in_size = left, out_size = sizeof chunk;

        tdefl_compress(d, in, &in_size, chunk, &out_size, TDEFL_SYNC_FLUSH);
        in += in_size;
        left -= in_size;
        collect(s, chunk, out_size);

        if (left == 0 && out_size < sizeof chunk) return;
    }
}

/* The stream so far, inflated: how many bytes it gives. */
static size_t inflated(const struct sink *s, uint8_t *out, size_t cap)
{
    mz_stream z;
    size_t got;

    memset(&z, 0, sizeof z);
    mz_inflateInit(&z);
    z.next_in = s->at;
    z.avail_in = (unsigned)s->n;
    z.next_out = out;
    z.avail_out = (unsigned)cap;
    mz_inflate(&z, MZ_SYNC_FLUSH);
    got = cap - z.avail_out;
    mz_inflateEnd(&z);
    return got;
}

/* `trials` pieces, each on a stream of its own: how many came back short. */
static int run(int old, int trials, size_t *big_out)
{
    size_t cap = 300000, big = 0;
    uint8_t *given = malloc(cap), *back = malloc(cap);
    int lost = 0, t;

    for (t = 1; t <= trials; t++) {
        tdefl_compressor d;
        struct sink s = { NULL, 0, 0 };
        size_t n;

        seed = (uint32_t)t;
        n = fill(given, cap);
        tdefl_init(&d, NULL, NULL, (int)tdefl_create_comp_flags_from_zip_params(6, MZ_DEFAULT_WINDOW_BITS,
                                                                              MZ_DEFAULT_STRATEGY));

        if (old) {
            old_rule(&d, given, n, &s);
        } else if (zsync_deflate(&d, given, n, collect, &s) != TDEFL_STATUS_OKAY) {
            lost++;
            continue;
        }

        if (s.n > 16384) big++;

        if (inflated(&s, back, cap) != n || memcmp(back, given, n) != 0) lost++;

        free(s.at);
    }

    *big_out = big;
    free(given); free(back);
    return lost;
}

/* And one stream, piece after piece, as `vncd` sends a connection's bands. */
static int one_stream(size_t *pieces_out)
{
    size_t cap = 4u << 20, total = 0, pieces = 0;
    uint8_t *given = malloc(cap), *back = malloc(cap);
    struct sink s = { NULL, 0, 0 };
    tdefl_compressor d;
    int wrong = 0;

    seed = 4242;
    tdefl_init(&d, NULL, NULL, (int)tdefl_create_comp_flags_from_zip_params(6, MZ_DEFAULT_WINDOW_BITS,
                                                                          MZ_DEFAULT_STRATEGY));

    while (total + 220000 < cap && !wrong) {
        size_t n = fill(given + total, cap - total);

        if (zsync_deflate(&d, given + total, n, collect, &s) != TDEFL_STATUS_OKAY) wrong = 1;

        total += n;
        pieces++;

        if (inflated(&s, back, cap) != total || memcmp(back, given, total) != 0) wrong = 1;
    }

    *pieces_out = pieces;
    free(given); free(back); free(s.at);
    return wrong;
}

int main(void)
{
    size_t big, old_big, pieces;
    int lost = run(0, 400, &big);
    int old_lost = run(1, 400, &old_big);
    int stream = one_stream(&pieces);

    if (lost || stream || big == 0 || old_lost == 0) {
        printf("FAIL: deflated and flushed whole - %d of 400 pieces short, one stream %s; "
               "%zu over a buffer; the old rule %d short%s\n", lost, stream ? "short" : "whole",
               big, old_lost, old_lost ? "" : ", so this test sees nothing");
        return 1;
    }

    printf("PASS: 400 pieces deflated and flushed whole, %zu of them over a 16 KB buffer, and "
           "%zu on one stream, each inflated back byte for byte; the old stopping rule left %d "
           "of the 400 short\n", big, pieces, old_lost);
    return 0;
}
