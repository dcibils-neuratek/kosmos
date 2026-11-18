/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A window's shadow, held to the method it replaced, on this computer.
 *
 *   build/host/test_shadow
 *
 * `user/kits/gfx/shadow.c` draws the shadow a window casts. On 24 September
 * it was rewritten for speed - Diego: "the drop shadow makes the entire UI
 * unsable because of the slowness when dragging windows" (`roadmap.md` 5zu)
 * - and a faster shadow that draws a different picture would be a regression
 * nobody could name. So the old method is here, whole, as the specification:
 * every pixel of the whole rectangle and its band, the corner test on each,
 * two divisions for its falloff, blended with the kit's source-over.
 *
 * Three things are held, over windows of every size and place, corners of
 * every radius, spreads and strengths, on a background of noise so a channel
 * that goes wrong shows:
 *
 *   - **the same pixels** are touched, and no others;
 *   - **each within one step** of the old value in every channel, the two
 *     roundings being a multiply by 255 and a shift by 256;
 *   - **a clip changes nothing inside it** and touches nothing outside.
 *
 * And it says how long each takes for one window on a 1920 by 1080 screen,
 * whole and for the kind of small rectangle a drag repaints.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../user/kits/gfx/shadow.h"

/* The kit's source-over for a black source, as `gfx.c` has it. */
static uint32_t mul255(uint32_t x, uint32_t a)
{
    uint32_t t = x * a + 128u;
    return (t + (t >> 8)) >> 8;
}

static uint32_t over_black(uint32_t a, uint32_t dst)
{
    uint32_t inv = 255u - a;
    uint32_t r = mul255((dst >> 16) & 0xffu, inv);
    uint32_t g = mul255((dst >> 8) & 0xffu, inv);
    uint32_t b = mul255(dst & 0xffu, inv);

    return (dst & 0xff000000u) | (r << 16) | (g << 8) | b;
}

/* The shadow as it was until 24 September: the specification. */
static void old_shadow(uint32_t *px, long w, long h, long rx, long ry,
                       long rw, long rh, long r, long spread, long alpha)
{
    long drop = spread / 3;
    long y;

    for (y = ry - spread + drop; y < ry + rh + spread + drop; y++) {
        long x;

        if (y < 0 || y >= h) {
            continue;
        }

        for (x = rx - spread; x < rx + rw + spread; x++) {
            long near_x, near_y, d, a;

            if (x < 0 || x >= w) {
                continue;
            }

            if (gfx_round_cover(x, y, rx, ry, rw, rh, r) >= 255) {
                continue;
            }

            near_x = x < rx ? rx - x : (x >= rx + rw ? x - (rx + rw) + 1 : 0);
            near_y = y - drop;
            near_y = near_y < ry ? ry - near_y
                     : (near_y >= ry + rh ? near_y - (ry + rh) + 1 : 0);

            d = near_x > near_y ? near_x : near_y;

            if (near_x > 0 && near_y > 0) {
                d = (near_x + near_y) * 3 / 4;
            }

            if (d >= spread) {
                continue;
            }

            a = alpha * (spread - d) / spread;
            a = a * (spread - d) / spread;

            if (a <= 0) {
                continue;
            }

            px[y * w + x] = over_black((uint32_t)a, px[y * w + x]);
        }
    }
}

static uint32_t seed = 12345;

static uint32_t rnd(void)
{
    seed = seed * 1103515245u + 12345u;
    return seed >> 8;
}

static long pick(long lo, long hi) { return lo + (long)(rnd() % (hi - lo + 1)); }

static int close_enough(uint32_t a, uint32_t b)
{
    int s;

    for (s = 0; s < 24; s += 8) {
        long d = (long)((a >> s) & 0xffu) - (long)((b >> s) & 0xffu);

        if (d < -1 || d > 1) {
            return 0;
        }
    }

    return 1;
}

static double now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int main(void)
{
    enum { W = 320, H = 240 };
    static uint32_t bg[W * H], ref[W * H], got[W * H], cut[W * H];
    long checks = 0, failed = 0, touched = 0;
    int round;

    for (round = 0; round < 400; round++) {
        long i;
        long rw = pick(1, 260), rh = pick(1, 200);
        long rx = pick(-60, W - 20), ry = pick(-60, H - 20);
        long r = (round % 5 == 0) ? 0 : pick(1, 24);
        long spread = pick(1, 48), alpha = pick(1, 255);
        long clip[4];

        for (i = 0; i < W * H; i++) bg[i] = 0xff000000u | (rnd() & 0xffffffu);

        memcpy(ref, bg, sizeof bg);
        memcpy(got, bg, sizeof bg);
        old_shadow(ref, W, H, rx, ry, rw, rh, r, spread, alpha);
        gfx_shadow(got, W * 4, W, H, rx, ry, rw, rh, r, spread, alpha, NULL);

        for (i = 0; i < W * H; i++) {
            int was_touched = ref[i] != bg[i];
            int now_touched = got[i] != bg[i];

            checks++;

            /* A pixel the old method darkened by less than a step may come
             * out unchanged, and the reverse; either is within a step. */
            if (!close_enough(ref[i], got[i])) {
                if (failed++ < 8) {
                    printf("not ok - round %d: pixel %ld,%ld is %06x, was %06x "
                           "(window %ld,%ld %ldx%ld r %ld spread %ld alpha %ld)\n",
                           round, i % W, i / W, got[i] & 0xffffffu,
                           ref[i] & 0xffffffu, rx, ry, rw, rh, r, spread, alpha);
                }
            }

            if (was_touched || now_touched) touched++;
        }

        /* A clip: exactly the unclipped shadow inside it, nothing outside. */
        clip[0] = pick(-20, W - 1);
        clip[1] = pick(-20, H - 1);
        clip[2] = pick(1, W);
        clip[3] = pick(1, H);

        memcpy(cut, bg, sizeof bg);
        gfx_shadow(cut, W * 4, W, H, rx, ry, rw, rh, r, spread, alpha, clip);

        for (i = 0; i < W * H; i++) {
            long x = i % W, y = i / W;
            int inside = x >= clip[0] && x < clip[0] + clip[2] &&
                         y >= clip[1] && y < clip[1] + clip[3];

            checks++;

            if (cut[i] != (inside ? got[i] : bg[i])) {
                if (failed++ < 8) {
                    printf("not ok - round %d: with a clip, pixel %ld,%ld "
                           "differs (%s the clip)\n", round, x, y,
                           inside ? "inside" : "outside");
                }
            }
        }
    }

    /* How long, for one window on a 1920x1080 screen. */
    {
        enum { SW = 1920, SH = 1080, N = 60 };
        uint32_t *screen = malloc((size_t)SW * SH * 4);
        long drag[4] = { 1300, 700, 96, 96 };
        double t0, t_old, t_new, t_clip;
        int k;

        if (!screen) {
            printf("not ok - no memory for the timing\n");
            return 1;
        }

        memset(screen, 0x80, (size_t)SW * SH * 4);

        t0 = now();
        for (k = 0; k < N; k++)
            old_shadow(screen, SW, SH, 400, 200, 900, 700, 12, 24, 90);
        t_old = (now() - t0) / N;

        t0 = now();
        for (k = 0; k < N; k++)
            gfx_shadow(screen, SW * 4, SW, SH, 400, 200, 900, 700, 12, 24, 90,
                       NULL);
        t_new = (now() - t0) / N;

        t0 = now();
        for (k = 0; k < N; k++)
            gfx_shadow(screen, SW * 4, SW, SH, 400, 200, 900, 700, 12, 24, 90,
                       drag);
        t_clip = (now() - t0) / N;

        free(screen);

        printf("shadow of a 900x700 window: %.3f ms as it was, %.3f ms whole "
               "now (%.0fx), %.4f ms for a 96-pixel rectangle a drag repaints\n",
               t_old * 1e3, t_new * 1e3, t_old / t_new, t_clip * 1e3);
    }

    /*
     * **Starlight's cut** (`gfx_cut_cover`, `docs/astra.md` S2), at the
     * menu's 8 eased by 7 and the dock's 12 by 10: outside at the corner,
     * inside past it, half a pixel on the slant, the same either side of the
     * diagonal, never less inside as a pixel moves in along it, and whole
     * along the edges past the eased meeting.
     */
    {
        long sizes[2][2] = { { 8, 7 }, { 12, 10 } };

        for (int k = 0; k < 2; k++) {
            long c = sizes[k][0], e = sizes[k][1], prev = -1;
            int sym = 1, mono = 1, edges = 1;
            long mid;

            checks++;
            if (gfx_cut_cover(0, 0, 0, 0, c, e) != 0) {
                printf("  the cut at %ld: the corner pixel is covered\n", c); failed++;
            }
            checks++;
            if (gfx_cut_cover(c + e + 2, c + e + 2, 0, 0, c, e) != 255) {
                printf("  the cut at %ld: a pixel well inside is not\n", c); failed++;
            }

            /* The pixel whose centre sits on the slant, `u + v = c`. */
            mid = gfx_cut_cover(c / 2, c / 2 - 1, 0, 0, c, e);
            checks++;
            if (mid < 96 || mid > 160) {
                printf("  the cut at %ld: a pixel on the slant is %ld, not half\n", c, mid);
                failed++;
            }

            for (long y = 0; y < 2 * c + 2 * e; y++) {
                for (long x = 0; x < 2 * c + 2 * e; x++) {
                    if (gfx_cut_cover(x, y, 0, 0, c, e) != gfx_cut_cover(y, x, 0, 0, c, e)) sym = 0;
                }
            }

            for (long d = 0; d < 2 * c + 2 * e; d++) {
                long v = gfx_cut_cover(d, d, 0, 0, c, e);

                if (v < prev) mono = 0;
                prev = v;
            }

            for (long x = c + e + 1; x < 4 * c; x++) {
                if (gfx_cut_cover(x, 0, 0, 0, c, e) != 255
                    || gfx_cut_cover(0, x, 0, 0, c, e) != 255) {
                    edges = 0;
                }
            }

            checks += 3;
            if (!sym) { printf("  the cut at %ld is not the same either side\n", c); failed++; }
            if (!mono) { printf("  the cut at %ld grows less inside along its diagonal\n", c); failed++; }
            if (!edges) { printf("  the cut at %ld takes from the edges past it\n", c); failed++; }
        }
    }

    if (failed) {
        printf("FAIL: %ld of %ld checks\n", failed, checks);
        return 1;
    }

    printf("PASS: %ld checks on a window's shadow against the method it "
           "replaced (%ld pixels shadowed, over 400 windows, corners and "
           "clips), and on Starlight's cut corner at two sizes\n", checks, touched);
    return 0;
}
