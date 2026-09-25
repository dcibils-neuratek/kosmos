/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A camera's YUY2, and a film's planes, into the screen's pixels, held on
 * the host.
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
 *
 * **And a film's three planes, 4:2:0** (`roadmap.md` 4e): each of the four
 * matrices an H.264 film is made with held to its floating-point
 * definition, and the vector path to the scalar one on random planes.
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

    /*
     * **A film's planes, 4:2:0, for the H.264 kit** (`roadmap.md` 4e).
     *
     * Each of the four matrices held to the floating-point specification it
     * is scaled from - Kr and Kb, studio or full range - within a step, over
     * every Y and a lattice of U and V; then the colours everybody knows;
     * then the vector path to the scalar one, bit for bit, on random planes
     * at widths that leave a scalar tail, odd heights, and strides wider
     * than the rows with sentinels past the last pixel.
     */
    {
        static const struct {
            const struct gfx_yuv_matrix *m;
            double kr, kb;
            int full;
            const char *name;
        } mats[] = {
            { &gfx_yuv_bt601,      0.299,  0.114,  0, "BT.601" },
            { &gfx_yuv_bt709,      0.2126, 0.0722, 0, "BT.709" },
            { &gfx_yuv_bt601_full, 0.299,  0.114,  1, "BT.601 full range" },
            { &gfx_yuv_bt709_full, 0.2126, 0.0722, 1, "BT.709 full range" },
        };
        unsigned k;

        for (k = 0; k < sizeof mats / sizeof mats[0]; k++) {
            double kr = mats[k].kr, kb = mats[k].kb, kg = 1 - kr - kb;
            double ys = mats[k].full ? 1.0 : 255.0 / 219.0;
            double cs = mats[k].full ? 1.0 : 255.0 / 224.0;
            double yo = mats[k].full ? 0 : 16;
            long wrong = 0;
            char what[128];

            for (y = 0; y < 256; y++) {
                for (u = 0; u < 256; u += 3) {
                    for (v = 0; v < 256; v += 5) {
                        uint8_t yy = (uint8_t)y, uu = (uint8_t)u, vv = (uint8_t)v;
                        uint32_t got;
                        double c = ys * (y - yo), d = cs * (u - 128);
                        double e = cs * (v - 128);
                        double r = c + 2 * (1 - kr) * e;
                        double b = c + 2 * (1 - kb) * d;
                        double g = c - (2 * kb * (1 - kb) / kg) * d
                                     - (2 * kr * (1 - kr) / kg) * e;
                        long rr = lround(r), gg = lround(g), bb = lround(b);

                        rr = rr < 0 ? 0 : rr > 255 ? 255 : rr;
                        gg = gg < 0 ? 0 : gg > 255 ? 255 : gg;
                        bb = bb < 0 ? 0 : bb > 255 ? 255 : bb;
                        gfx_i420_scalar(&got, 4, &yy, &uu, &vv, 1, 1, 1, 1, 1,
                                        mats[k].m);
                        wrong += !near(got, 0xff000000u | (uint32_t)rr << 16
                                            | (uint32_t)gg << 8
                                            | (uint32_t)bb);
                    }
                }
            }

            snprintf(what, sizeof what, "planes, %s: every Y and a lattice "
                     "of U and V within a step (%ld were not)",
                     mats[k].name, wrong);
            check(wrong == 0, what);
        }

        {
            uint8_t yy = 16, mid = 128, full_white = 255, studio_white = 235;
            uint32_t got;

            gfx_i420(&got, 4, &yy, &mid, &mid, 1, 1, 1, 1, 1, &gfx_yuv_bt709);
            check(got == 0xff000000u, "planes, BT.709: 16,128,128 is black");
            gfx_i420(&got, 4, &studio_white, &mid, &mid, 1, 1, 1, 1, 1,
                     &gfx_yuv_bt709);
            check(got == 0xffffffffu, "planes, BT.709: 235,128,128 is white");
            gfx_i420(&got, 4, &full_white, &mid, &mid, 1, 1, 1, 1, 1,
                     &gfx_yuv_bt601_full);
            check(got == 0xffffffffu, "planes, full range: 255,128,128 is "
                                      "white");
        }

        {
            static const unsigned widths[] = { 1, 2, 7, 8, 15, 16, 17, 31, 32,
                                               33, 100, 639, 640, 1920 };
            static const unsigned heights[] = { 1, 2, 3, 7 };
            unsigned seed = 424242u, w, h, i;
            int same = 1, spared = 1;

            for (w = 0; w < sizeof widths / sizeof widths[0]; w++) {
                for (h = 0; h < sizeof heights / sizeof heights[0]; h++) {
                    unsigned fw = widths[w], fh = heights[h];
                    unsigned ys_ = fw + 32, cs_ = (fw + 1) / 2 + 16;
                    unsigned ch = (fh + 1) / 2, pitch = fw + 4;
                    uint8_t *yp = malloc((size_t)ys_ * fh);
                    uint8_t *up = malloc((size_t)cs_ * ch);
                    uint8_t *vp = malloc((size_t)cs_ * ch);
                    uint32_t *a = malloc((size_t)pitch * fh * 4);
                    uint32_t *b = malloc((size_t)pitch * fh * 4);

                    for (i = 0; i < ys_ * fh; i++) {
                        seed = seed * 1103515245u + 12345u;
                        yp[i] = (uint8_t)(seed >> 16);
                    }
                    for (i = 0; i < cs_ * ch; i++) {
                        seed = seed * 1103515245u + 12345u;
                        up[i] = (uint8_t)(seed >> 16);
                        vp[i] = (uint8_t)(seed >> 8);
                    }

                    for (k = 0; k < sizeof mats / sizeof mats[0]; k++) {
                        for (i = 0; i < pitch * fh; i++) a[i] = b[i] = 0x12345678u;

                        gfx_i420(a, pitch * 4, yp, up, vp, ys_, cs_, cs_, fw,
                                 fh, mats[k].m);
                        gfx_i420_scalar(b, pitch * 4, yp, up, vp, ys_, cs_,
                                        cs_, fw, fh, mats[k].m);
                        same &= memcmp(a, b, (size_t)pitch * fh * 4) == 0;

                        for (row = 0; row < (int)fh; row++) {
                            for (x = (int)fw; x < (int)pitch; x++) {
                                spared &= a[row * pitch + x] == 0x12345678u;
                            }
                        }
                    }

                    free(yp);
                    free(up);
                    free(vp);
                    free(a);
                    free(b);
                }
            }

            check(same, "planes: the vector path equal to the scalar one, "
                        "widths 1 to 1920, every matrix");
            check(spared, "planes: nothing written past a row's end");
        }

        {
            enum { FW = 1920, FH = 1080, N = 20 };
            uint8_t *yp = malloc(FW * FH), *up = malloc(FW * FH / 4);
            uint8_t *vp = malloc(FW * FH / 4);
            uint32_t *dst = malloc((size_t)FW * FH * 4);
            double t0, t_scalar, t_vec;
            int n;

            memset(yp, 100, FW * FH);
            memset(up, 90, FW * FH / 4);
            memset(vp, 200, FW * FH / 4);

            t0 = now();
            for (n = 0; n < N; n++)
                gfx_i420_scalar(dst, FW * 4, yp, up, vp, FW, FW / 2, FW / 2,
                                FW, FH, &gfx_yuv_bt709);
            t_scalar = (now() - t0) / N;

            t0 = now();
            for (n = 0; n < N; n++)
                gfx_i420(dst, FW * 4, yp, up, vp, FW, FW / 2, FW / 2, FW, FH,
                         &gfx_yuv_bt709);
            t_vec = (now() - t0) / N;

            printf("a 1920x1080 film frame from planes: %.3f ms scalar, "
                   "%.3f ms vector (%.1fx)\n", t_scalar * 1e3, t_vec * 1e3,
                   t_scalar / t_vec);
            free(yp);
            free(up);
            free(vp);
            free(dst);
        }
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

    printf("PASS: %d checks on YUY2 and 4:2:0 planes into the screen's "
           "pixels (every Y, U and V against BT.601, the four matrices a "
           "film uses, the known colours, a frame straight and mirrored, and "
           "the %s path bit for bit with the scalar one)\n",
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
