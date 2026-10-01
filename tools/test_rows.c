/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `gfx_fill_row` and `gfx_cover_row` against themselves a pixel at a time
 * (`user/kits/gfx/rows.c`, `roadmap.md` 6zz h).
 *
 *   - every coverage over every pair of ink and ground, channel values 0 to
 *     255 all three: the lanes, the scalar loop and `gfx.c`'s `mix` written
 *     out here agree - 16.7 million mixes, which is all of them;
 *   - random rows of every width from 0 to 40, so each tail is taken, with
 *     runs of nothing and of whole ink in them as a glyph has, and nothing
 *     past a row's end touched;
 *   - fills of every width the same way;
 *   - and a screen's worth of each timed both ways, printed.
 *
 * Built twice, as `tools/test_pack.c` is: natively, which is NEON on this
 * Mac, and `-arch x86_64`, which Rosetta runs as SSE2 - and with clang's own
 * vectorisers off, so that the scalar loop being timed is one.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rows.h"

static int failures, checks;

static void check(bool ok, const char *what)
{
    checks++;

    if (!ok) {
        failures++;
        printf("not ok %d - %s\n", checks, what);
    }
}

static uint32_t seed = 0x51C0FFEEu;

static uint32_t next(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static double now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

/* `gfx.c`'s `mix`, as it is written there - the reference of the reference. */
static uint32_t mix(uint32_t dst, uint32_t src, unsigned a)
{
    unsigned inv = 255u - a;
    unsigned r = ((((src >> 16) & 0xff) * a) + (((dst >> 16) & 0xff) * inv)
                  + 127) / 255;
    unsigned g = ((((src >> 8) & 0xff) * a) + (((dst >> 8) & 0xff) * inv)
                  + 127) / 255;
    unsigned b = ((((src) & 0xff) * a) + (((dst) & 0xff) * inv) + 127) / 255;

    return 0xff000000u | (r << 16) | (g << 8) | b;
}

int main(void)
{
    /* Every coverage, every ink, every ground. */
    {
        static uint32_t vec[256], one[256];
        static uint8_t cover[256];
        bool same = true, right = true;
        unsigned a, ink;

        for (a = 0; a < 256 && same && right; a++) {
            memset(cover, (int)a, sizeof(cover));

            for (ink = 0; ink < 256 && same && right; ink++) {
                uint32_t colour = 0xff000000u | ink << 16 | ink << 8 | ink;
                unsigned k;

                for (k = 0; k < 256; k++) {
                    vec[k] = one[k] = 0xff000000u | k << 16 | k << 8 | k;
                }

                gfx_cover_row(vec, cover, 256, colour, true);
                gfx_cover_row(one, cover, 256, colour, false);

                for (k = 0; k < 256; k++) {
                    uint32_t ground = 0xff000000u | k << 16 | k << 8 | k;
                    uint32_t want = a == 0 ? ground : a == 255 ? colour
                                    : mix(ground, colour, a);

                    if (vec[k] != one[k] && same) {
                        printf("  coverage %u, ink %u, ground %u: four at a "
                               "time %08x, one at a time %08x\n", a, ink, k,
                               vec[k], one[k]);
                        same = false;
                    }

                    if (one[k] != want && right) {
                        printf("  coverage %u, ink %u, ground %u: %08x, "
                               "mix says %08x\n", a, ink, k, one[k], want);
                        right = false;
                    }
                }
            }
        }

        check(same, "a glyph's coverage in the lanes is the scalar loop's, "
                    "every coverage over every ink and ground");
        check(right, "and the scalar loop is gfx.c's mix, 0 and 255 its "
                     "two ends");
    }

    /* Random rows of every width, and nothing past their end. */
    {
        bool same = true, kept = true;
        long w;

        for (w = 0; w <= 40; w++) {
            int round;

            for (round = 0; round < 50; round++) {
                uint32_t vec[48], one[48], ink = next();
                uint8_t cover[48];
                long k;

                for (k = 0; k < 48; k++) {
                    unsigned pick = next() % 4;

                    vec[k] = one[k] = next();
                    cover[k] = pick == 0 ? 0 : pick == 1 ? 255
                               : (uint8_t)next();
                }

                gfx_cover_row(vec, cover, w, ink, true);
                gfx_cover_row(one, cover, w, ink, false);
                same = same && memcmp(vec, one, sizeof(vec)) == 0;

                for (k = w; k < 48; k++) {
                    uint32_t before = one[k];

                    kept = kept && vec[k] == before;
                }
            }
        }

        check(same, "random glyph rows of every width from 0 to 40 agree to "
                    "the bit, four at a time and one");
        check(kept, "and no pixel past a row's end is touched");
    }

    /* Fills of every width. */
    {
        bool same = true, kept = true;
        long w;

        for (w = 0; w <= 40; w++) {
            uint32_t vec[48], one[48], colour = next();
            long k;

            for (k = 0; k < 48; k++) {
                vec[k] = one[k] = 0x5a5a5a5au;
            }

            gfx_fill_row(vec, w, colour, true);
            gfx_fill_row(one, w, colour, false);
            same = same && memcmp(vec, one, sizeof(vec)) == 0;

            for (k = 0; k < 48; k++) {
                kept = kept && vec[k] == (k < w ? colour : 0x5a5a5a5au);
            }
        }

        check(same, "fills of every width from 0 to 40 agree");
        check(kept, "and a fill covers its width and not a pixel more");
    }

    /* A screen's worth of each, both ways. */
    {
        enum { W = 1920, H = 1080, ROUNDS = 8 };
        uint32_t *px = malloc((size_t)W * H * 4);
        uint8_t *cover = malloc((size_t)W * H);
        double fill[2] = { 0, 0 }, text[2] = { 0, 0 };
        int way, round;
        long k;

        /* Coverage as text has it: mostly nothing, some ink whole, the
         * edges between. */
        for (k = 0; k < (long)W * H; k++) {
            unsigned pick = next() % 8;

            cover[k] = pick < 5 ? 0 : pick == 5 ? 255 : (uint8_t)next();
        }

        for (way = 0; way < 2; way++) {
            for (round = 0; round < ROUNDS; round++) {
                double t0 = now_ms();
                int y;

                for (y = 0; y < H; y++) {
                    gfx_fill_row(px + (size_t)y * W, W, 0xffeef2fbu,
                                 way == 0);
                }

                fill[way] += now_ms() - t0;
                t0 = now_ms();

                for (y = 0; y < H; y++) {
                    gfx_cover_row(px + (size_t)y * W, cover + (size_t)y * W,
                                  W, 0xff1c1c1cu, way == 0);
                }

                text[way] += now_ms() - t0;
            }
        }

        printf("  a 1920x1080 fill: %.2f ms four at a time, %.2f ms one at a "
               "time (%.1fx)\n", fill[0] / ROUNDS, fill[1] / ROUNDS,
               fill[0] > 0 ? fill[1] / fill[0] : 0.0);
        printf("  1920x1080 of a glyph's coverage laid over: %.2f ms four at "
               "a time, %.2f ms one at a time (%.1fx)\n", text[0] / ROUNDS,
               text[1] / ROUNDS, text[0] > 0 ? text[1] / text[0] : 0.0);
        free(px);
        free(cover);
    }

    if (failures == 0) {
        printf("PASS: %d checks on filling and on a glyph's coverage, a row "
               "at a time, on this machine.\n", checks);
        return 0;
    }

    printf("FAIL: %d of %d checks on filling and on a glyph's coverage.\n",
           failures, checks);
    return 1;
}
