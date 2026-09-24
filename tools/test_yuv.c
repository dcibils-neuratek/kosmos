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

    /* How long a C920 frame takes, 640 by 480. */
    {
        enum { FW = 640, FH = 480, N = 200 };
        uint8_t *src = malloc(FW * FH * 2);
        uint32_t *dst = malloc(FW * FH * 4);
        double t0, t;
        int k;

        for (k = 0; k < FW * FH * 2; k++) src[k] = (uint8_t)(k * 7);

        t0 = now();
        for (k = 0; k < N; k++) gfx_yuy2(dst, FW * 4, src, FW, FH, k & 1);
        t = (now() - t0) / N;

        printf("a 640x480 frame: %.3f ms here\n", t * 1e3);
        free(src);
        free(dst);
    }

    if (fails) {
        printf("FAIL: %d of %d checks on YUY2\n", fails, checks + fails);
        return 1;
    }

    printf("PASS: %d checks on YUY2 into the screen's pixels (every Y, U and "
           "V against BT.601, the known colours, a frame straight and "
           "mirrored)\n", checks);
    return 0;
}
