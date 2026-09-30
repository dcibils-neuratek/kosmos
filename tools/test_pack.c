/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `gfx_pack_row` against itself a pixel at a time (`user/kits/gfx/pack.c`).
 *
 * The vector loop and the scalar one have to be one answer to the bit, since
 * a VNC viewer draws whichever it is sent. So:
 *
 *   - the identity the lanes use for dividing by 255, for every channel
 *     value and every largest value up to 255, against the division;
 *   - every row width from 0 to 40, so each tail length is taken, in each
 *     format a viewer can ask for - 32 bits both ways round, 16 bits as
 *     565 and 555 both ways round, 8 bits as 332, a ten-bit channel that
 *     has to go the scalar way, and this surface's own - over pixels from
 *     a fixed seed;
 *   - and the time a 1920x1080 frame takes each way, printed, because the
 *     vector path exists to be faster and a test is where that is seen.
 *
 * Built twice, as `tools/test_yuv.c` is: natively, which is NEON on this
 * Mac, and `-arch x86_64`, which Rosetta runs as SSE2.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pack.h"

static int failures, checks;

static uint32_t seed = 0x2F6B3A91u;

static uint32_t next(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static double now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void check_identity(void)
{
    uint32_t c, max;

    checks++;

    for (max = 1; max <= 255; max++) {
        for (c = 0; c <= 255; c++) {
            uint32_t t = c * max + 128u;
            uint32_t lanes = (t + (t >> 8)) >> 8;
            uint32_t divided = (c * max + 127u) / 255u;

            if (lanes != divided) {
                failures++;
                printf("FAIL: dividing by 255 in the lanes: c %u, max %u gives "
                       "%u where the division gives %u\n", c, max, lanes, divided);
                return;
            }
        }
    }
}

static void check_rows(const char *name, struct gfx_pack_format f)
{
    uint32_t src[40];
    uint8_t vector[40 * 4 + 16], scalar[40 * 4 + 16];
    unsigned width, i;

    checks++;

    for (width = 0; width <= 40; width++) {
        for (i = 0; i < width; i++) src[i] = next() | 0xFF000000u;

        memset(vector, 0xAA, sizeof(vector));
        memset(scalar, 0xAA, sizeof(scalar));
        gfx_pack_row(src, vector, width, &f);
        gfx_pack_row_scalar(src, scalar, width, &f);

        /* The bytes after the row too: neither may write past it. */
        if (memcmp(vector, scalar, sizeof(vector)) != 0) {
            failures++;
            printf("FAIL: %s, a row of %u: the vector loop and the scalar one "
                   "differ\n", name, width);
            return;
        }
    }
}

static void time_frame(const char *name, struct gfx_pack_format f)
{
    enum { W = 1920, H = 1080 };
    uint32_t *frame = malloc((size_t)W * H * 4);
    uint8_t *out = malloc((size_t)W * H * 4);
    double began, vector, scalar;
    unsigned y, i;

    for (i = 0; i < W * H; i++) frame[i] = next() | 0xFF000000u;

    /* Once untimed, so neither is charged for the pages being touched. */
    for (y = 0; y < H; y++) gfx_pack_row_scalar(frame + y * W, out + (size_t)y * W * (f.bpp / 8), W, &f);

    began = now();
    for (y = 0; y < H; y++) gfx_pack_row(frame + y * W, out + (size_t)y * W * (f.bpp / 8), W, &f);
    vector = now() - began;

    began = now();
    for (y = 0; y < H; y++) gfx_pack_row_scalar(frame + y * W, out + (size_t)y * W * (f.bpp / 8), W, &f);
    scalar = now() - began;

    printf("  %-22s %6.2f ms a 1920x1080 frame, %6.2f ms a pixel at a time "
           "(%.1fx)\n", name, vector * 1e3, scalar * 1e3, scalar / vector);

    free(frame);
    free(out);
}

int main(void)
{
    static const struct { const char *name; struct gfx_pack_format f; } formats[] = {
        { "this surface's own",  { 32, false, 255, 255, 255, 16, 8, 0 } },
        { "32 bits, BGR",        { 32, false, 255, 255, 255, 0, 8, 16 } },
        { "32 bits, big-endian", { 32, true,  255, 255, 255, 16, 8, 0 } },
        { "16 bits, 565",        { 16, false, 31, 63, 31, 11, 5, 0 } },
        { "16 bits, 565 big",    { 16, true,  31, 63, 31, 11, 5, 0 } },
        { "16 bits, 555",        { 16, false, 31, 31, 31, 10, 5, 0 } },
        { "8 bits, 332",         { 8,  false, 7, 7, 3, 0, 3, 6 } },
        { "ten bits a channel",  { 32, false, 1023, 1023, 1023, 20, 10, 0 } },
    };
    unsigned i;

    check_identity();

    for (i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        check_rows(formats[i].name, formats[i].f);
    }

    if (failures) {
        printf("FAIL: %d of %d checks on packing pixels for a viewer\n",
               failures, checks);
        return 1;
    }

    printf("PASS: %d checks on packing pixels for a viewer (dividing by 255 in "
           "the lanes for every channel and largest value, and every row "
           "width to 40 in %u formats, the vector loop against the scalar)\n",
           checks, (unsigned)(sizeof(formats) / sizeof(formats[0])));

    time_frame(formats[0].name, formats[0].f);
    time_frame(formats[3].name, formats[3].f);
    time_frame(formats[2].name, formats[2].f);
    return 0;
}
