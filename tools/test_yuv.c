/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A camera's YUY2 into the screen's pixels, held on the host.
 *
 * `user/kits/gfx/yuv.c` converts every pixel of every frame, so it is held
 * to the obvious way of doing it - BT.601 studio range in floating point,
 * one pixel at a time - over every Y, U and V there is, within one step in
 * each channel; to the colours everybody knows the values of; and to a
 * frame, straight and mirrored, with a pitch wider than the picture so a
 * write past a row's end shows. And it says how long a 640x480 frame takes.
 *
 * **And the vector paths to the scalar one, bit for bit** (`roadmap.md`
 * 5zw): every Y, U and V through `gfx_yuy2` itself, straight and mirrored,
 * and random frames at widths that leave the scalar tail something to do.
 * Built twice - natively, which on this Mac is NEON, and with `-arch
 * x86_64`, which runs under Rosetta and is SSE2 - so both are held here
 * and not only in the gate.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../user/kits/gfx/yuv.h"

static int checks;
static int fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  not ok: %s\n", what);
    }
}

/* The specification: BT.601, studio range, in floating point. */
static uint32_t reference(int y, int u, int v)
{
    double yy = 1.164383 * (y - 16);
    double r = yy + 1.596027 * (v - 128);
    double g = yy - 0.391762 * (u - 128) - 0.812968 * (v - 128);
    double b = yy + 2.017232 * (u - 128);
    long rr = lround(r), gg = lround(g), bb = lround(b);

    rr = rr < 0 ? 0 : rr > 255 ? 255 : rr;
    gg = gg < 0 ? 0 : gg > 255 ? 255 : gg;
    bb = bb < 0 ? 0 : bb > 255 ? 255 : bb;
    return 0xff000000u | (uint32_t)rr << 16 | (uint32_t)gg << 8 | (uint32_t)bb;
}

static int near(uint32_t a, uint32_t b)
{
    int s;

    for (s = 0; s < 24; s += 8) {
        int d = (int)((a >> s) & 0xff) - (int)((b >> s) & 0xff);

        if (d < -1 || d > 1) {
            return 0;
        }
    }

    return (a >> 24) == 0xff;
}

static double now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int main(void)
{
    enum { W = 6, H = 2, PITCH = 10 };      /* four words past each row */
    static const uint8_t pair[4] = { 81, 90, 145, 240 };
    uint8_t frame[W * 2 * H];
    uint32_t out[PITCH * H], want[W * H];
    long far = 0;
    int y, u, v, x, row;

    /* Every Y, U and V, one pixel at a time. */
    for (y = 0; y < 256; y++) {
        for (u = 0; u < 256; u++) {
            for (v = 0; v < 256; v++) {
                if (!near(gfx_yuv_pixel(y, u, v), reference(y, u, v))) {
                    far++;
                }
            }
        }
    }

    {
        char what[96];

        snprintf(what, sizeof what, "all 16,777,216 within a step of BT.601 "
                 "(%ld were not)", far);
        check(far == 0, what);
    }

    /* The colours everybody knows. */
    check(gfx_yuv_pixel(16, 128, 128) == 0xff000000u, "black is 16,128,128");
    check(gfx_yuv_pixel(235, 128, 128) == 0xffffffffu, "white is 235,128,128");
    check(near(gfx_yuv_pixel(81, 90, 240), 0xffff0000u), "red is 81,90,240");
    check(near(gfx_yuv_pixel(145, 54, 34), 0xff00ff00u), "green is 145,54,34");
    check(near(gfx_yuv_pixel(41, 240, 110), 0xff0000ffu), "blue is 41,240,110");

    /* A frame: each pair is Y0 U Y1 V, the two sharing U and V. */
    for (row = 0; row < H; row++) {
        for (x = 0; x < W / 2; x++) {
            uint8_t *p = frame + row * W * 2 + x * 4;

            p[0] = (uint8_t)(pair[0] + 10 * x + row);
            p[1] = pair[1];
            p[2] = (uint8_t)(pair[2] - 10 * x);
            p[3] = (uint8_t)(pair[3] - 20 * row);

            want[row * W + 2 * x] = gfx_yuv_pixel(p[0], p[1], p[3]);
            want[row * W + 2 * x + 1] = gfx_yuv_pixel(p[2], p[1], p[3]);
        }
    }

    for (x = 0; x < PITCH * H; x++) out[x] = 0x12345678u;
    gfx_yuy2(out, PITCH * 4, frame, W, H, false);

    {
        int ok = 1, spared = 1;

        for (row = 0; row < H; row++) {
            for (x = 0; x < W; x++) {
                ok &= out[row * PITCH + x] == want[row * W + x];
            }

            for (x = W; x < PITCH; x++) {
                spared &= out[row * PITCH + x] == 0x12345678u;
            }
        }

        check(ok, "a frame, pixel for pixel, each pair sharing its colour");
        check(spared, "nothing written past a row's end");
    }

    for (x = 0; x < PITCH * H; x++) out[x] = 0x12345678u;
    gfx_yuy2(out, PITCH * 4, frame, W, H, true);

    {
        int ok = 1, spared = 1;

        for (row = 0; row < H; row++) {
            for (x = 0; x < W; x++) {
                ok &= out[row * PITCH + x] == want[row * W + (W - 1 - x)];
            }

            for (x = W; x < PITCH; x++) {
                spared &= out[row * PITCH + x] == 0x12345678u;
            }
        }

        check(ok, "mirrored, the same pixels right to left");
        check(spared, "and nothing written past a row's end");
    }

    /*
     * Every Y, U and V through `gfx_yuy2` - the vector path - as 65,536 rows
     * of 256 pixels, one row for each U and V and every Y along it, straight
     * and mirrored, each pixel held to the one-pixel function.
     */
    {
        enum { RW = 256 };
        static uint8_t line[RW * 2];
        static uint32_t got[RW];
        long wrong = 0, wrong_mirrored = 0;

        for (u = 0; u < 256; u++) {
            for (v = 0; v < 256; v++) {
                for (x = 0; x < RW / 2; x++) {
                    line[x * 4] = (uint8_t)(2 * x);
                    line[x * 4 + 1] = (uint8_t)u;
                    line[x * 4 + 2] = (uint8_t)(2 * x + 1);
                    line[x * 4 + 3] = (uint8_t)v;
                }

                gfx_yuy2(got, sizeof(got), line, RW, 1, false);

                for (x = 0; x < RW; x++) {
                    wrong += got[x] != gfx_yuv_pixel(x, u, v);
                }

                gfx_yuy2(got, sizeof(got), line, RW, 1, true);

                for (x = 0; x < RW; x++) {
                    wrong_mirrored += got[RW - 1 - x] != gfx_yuv_pixel(x, u, v);
                }
            }
        }

        check(wrong == 0, "every Y, U and V through gfx_yuy2, the pixel "
                          "gfx_yuv_pixel gives");
        check(wrong_mirrored == 0, "and mirrored, the same pixels right to "
                                   "left");
    }

    /*
     * Random frames at widths that leave pairs for the scalar tail - two to
     * 642 pixels - with four sentinel words past each row, to the scalar
     * path, straight and mirrored.
     */
    {
        static const unsigned widths[] = { 2, 6, 14, 16, 18, 30, 32, 34, 70,
                                           126, 640, 642 };
        unsigned seed = 1234567u, w, k;
        int same = 1, spared = 1;

        for (k = 0; k < sizeof(widths) / sizeof(widths[0]); k++) {
            unsigned fw = widths[k], fh = 5, fp = fw + 4, i, m;
            uint8_t *src = malloc(fw * 2 * fh);
            uint32_t *a = malloc(fp * fh * 4), *b = malloc(fp * fh * 4);

            for (i = 0; i < fw * 2 * fh; i++) {
                seed = seed * 1103515245u + 12345u;
                src[i] = (uint8_t)(seed >> 16);
            }

            for (m = 0; m < 2; m++) {
                for (i = 0; i < fp * fh; i++) a[i] = b[i] = 0x12345678u;

                gfx_yuy2(a, fp * 4, src, fw, fh, m != 0);
                gfx_yuy2_scalar(b, fp * 4, src, fw, fh, m != 0);

                for (row = 0; row < (int)fh; row++) {
                    for (w = 0; w < fp; w++) {
                        uint32_t pa = a[row * fp + w], pb = b[row * fp + w];

                        if (w < fw) {
                            same &= pa == pb;
                        } else {
                            spared &= pa == 0x12345678u && pb == 0x12345678u;
                        }
                    }
                }
            }

            free(src);
            free(a);
            free(b);
        }

        check(same, "random frames 2 to 642 pixels wide, the vector path "
                    "equal to the scalar one, straight and mirrored");
        check(spared, "and neither writing past a row's end");
    }

    /*
     * **Into planes, 4:2:0, for the encoder** (`roadmap.md` 6d 8f). A small
     * frame worked out by hand: every Y as it came, a U and a V for each two
     * by two, the rounded mean of the two rows'.
     */
    {
        /* 4 by 3, so the last row pairs with itself. */
        static const uint8_t f[3][8] = {
            { 10, 100, 11, 200,  12, 101, 13, 201 },
            { 20,  51, 21, 150,  22,  60, 23, 160 },
            { 30,  90, 31, 190,  32,  92, 33, 192 },
        };
        uint8_t y[4 * 3], u[2 * 2], v[2 * 2];

        memset(y, 0xEE, sizeof y);
        memset(u, 0xEE, sizeof u);
        memset(v, 0xEE, sizeof v);
        gfx_yuy2_i420(y, u, v, 4, 2, &f[0][0], 4, 3);

        check(y[0] == 10 && y[1] == 11 && y[2] == 12 && y[3] == 13
              && y[4] == 20 && y[7] == 23 && y[8] == 30 && y[11] == 33,
              "every Y into the Y plane, as it came");
        check(u[0] == 76 && u[1] == 81 && v[0] == 175 && v[1] == 181,
              "U and V the rounded mean of two rows: (100 + 51 + 1) / 2 = 76");
        check(u[2] == 90 && u[3] == 92 && v[2] == 190 && v[3] == 192,
              "and an odd last row its own pair");
    }

    /*
     * The vector path to the scalar one, bit for bit: random frames, widths
     * that leave pairs for the scalar tail, odd and even heights, strides
     * wider than the rows with sentinels in the gap.
     */
    {
        static const unsigned widths[] = { 2, 6, 14, 16, 30, 32, 34, 64, 66,
                                           126, 640, 642 };
        static const unsigned heights[] = { 1, 2, 3, 5, 8 };
        unsigned seed = 7654321u, k, h;
        int same = 1, spared = 1;

        for (k = 0; k < sizeof(widths) / sizeof(widths[0]); k++) {
            for (h = 0; h < sizeof(heights) / sizeof(heights[0]); h++) {
                unsigned fw = widths[k], fh = heights[h], i;
                unsigned ys = fw + 8, cs = fw / 2 + 8, ch = (fh + 1) / 2;
                size_t yb = (size_t)ys * fh, cb = (size_t)cs * ch;
                uint8_t *src = malloc(fw * 2 * fh);
                uint8_t *a = malloc(yb + 2 * cb), *b = malloc(yb + 2 * cb);

                for (i = 0; i < fw * 2 * fh; i++) {
                    seed = seed * 1103515245u + 12345u;
                    src[i] = (uint8_t)(seed >> 16);
                }

                memset(a, 0xA5, yb + 2 * cb);
                memset(b, 0xA5, yb + 2 * cb);
                gfx_yuy2_i420(a, a + yb, a + yb + cb, ys, cs, src, fw, fh);
                gfx_yuy2_i420_scalar(b, b + yb, b + yb + cb, ys, cs, src, fw,
                                     fh);
                same &= memcmp(a, b, yb + 2 * cb) == 0;

                for (i = 0; i < fh; i++) {
                    spared &= a[i * ys + fw] == 0xA5 && a[i * ys + ys - 1] == 0xA5;
                }

                for (i = 0; i < ch; i++) {
                    spared &= a[yb + i * cs + fw / 2] == 0xA5
                              && a[yb + cb + i * cs + fw / 2] == 0xA5;
                }

                free(src);
                free(a);
                free(b);
            }
        }

        check(same, "into planes: the vector path equal to the scalar one, "
                    "widths 2 to 642, heights 1 to 8");
        check(spared, "and nothing written past a row's end in any plane");
    }

    /* How long a C920 frame takes, 640 by 480. */
    {
        enum { FW = 640, FH = 480, N = 200 };
        uint8_t *src = malloc(FW * FH * 2);
        uint32_t *dst = malloc(FW * FH * 4);
        double t0, t;
        int k;

        for (k = 0; k < FW * FH * 2; k++) src[k] = (uint8_t)(k * 7);

        double t_scalar;

        t0 = now();
        for (k = 0; k < N; k++) gfx_yuy2_scalar(dst, FW * 4, src, FW, FH, k & 1);
        t_scalar = (now() - t0) / N;

        t0 = now();
        for (k = 0; k < N; k++) gfx_yuy2(dst, FW * 4, src, FW, FH, k & 1);
        t = (now() - t0) / N;

        {
            uint8_t *planes = malloc(FW * FH * 3 / 2);
            double p_scalar, p_vec;

            t0 = now();
            for (k = 0; k < N; k++)
                gfx_yuy2_i420_scalar(planes, planes + FW * FH,
                                     planes + FW * FH * 5 / 4, FW, FW / 2,
                                     src, FW, FH);
            p_scalar = (now() - t0) / N;

            t0 = now();
            for (k = 0; k < N; k++)
                gfx_yuy2_i420(planes, planes + FW * FH,
                              planes + FW * FH * 5 / 4, FW, FW / 2, src, FW,
                              FH);
            p_vec = (now() - t0) / N;

            printf("into planes: %.3f ms scalar, %.3f ms vector (%.1fx)\n",
                   p_scalar * 1e3, p_vec * 1e3, p_scalar / p_vec);
            free(planes);
        }

        printf("a 640x480 frame: %.3f ms one pair at a time, %.3f ms with %s "
               "(%.1fx)\n", t_scalar * 1e3, t * 1e3,
#if defined(__ARM_NEON)
               "NEON",
#elif defined(__SSE2__)
               "SSE2",
#else
               "no vectors",
#endif
               t_scalar / t);
        free(src);
        free(dst);
    }

    if (fails) {
        printf("FAIL: %d of %d checks on YUY2\n", fails, checks + fails);
        return 1;
    }

    printf("PASS: %d checks on YUY2 into the screen's pixels (every Y, U and "
           "V against BT.601, the known colours, a frame straight and "
           "mirrored, and the %s path bit for bit with the scalar one)\n",
           checks,
#if defined(__ARM_NEON)
           "NEON"
#elif defined(__SSE2__)
           "SSE2"
#else
           "scalar"
#endif
           );
    return 0;
}
