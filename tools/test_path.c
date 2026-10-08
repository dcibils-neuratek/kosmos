/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * gfx's paths, on the Mac (`user/kits/gfx/path.c`, `docs/maps.md` M1): the
 * coverage a shape leaves, held to areas worked out by hand. A square on
 * the pixel grid is solid inside and nothing outside; moved half a pixel,
 * its edges are half; a triangle's coverage sums to its area; a hole
 * cancels its ring; two rings wound alike are their union; a wide line
 * covers its width and its round ends; a shape off the box's left edge
 * still covers from that edge, and one off its right does nothing; and the
 * buffer reads back empty after every path.
 */

#include "path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  %s\n", what);
    }
}

#define W 40
#define H 30

static uint8_t pic[H][W];

static uint32_t pixels[H][W];

/* Painted white over nothing at all: what is left in each pixel's alpha is
 * the shape's coverage, 0..255. */
static void read_out(struct gfx_path *p)
{
    memset(pixels, 0, sizeof pixels);
    gfx_path_paint(p, &pixels[0][0], W * 4, 0xffffff);

    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) pic[y][x] = (uint8_t)(pixels[y][x] >> 24);
}

static long total(void)
{
    long sum = 0;

    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) sum += pic[y][x];

    return sum;
}

static int near(long got, long want, long slack)
{
    return got >= want - slack && got <= want + slack;
}

int main(void)
{
    struct gfx_path p = { 0 };
    char said[200];

    /* 1. A square on the grid: 10..20 by 5..15. */
    {
        float sq[] = { 10, 5, 20, 5, 20, 15, 10, 15 };

        check(gfx_path_begin(&p, 0, 0, W, H) == 0, "a path's buffer was refused");
        gfx_path_ring(&p, sq, 4);
        read_out(&p);
        check(pic[10][15] == 255 && pic[5][10] == 255 && pic[14][19] == 255,
              "a square on the grid is not solid inside");
        check(pic[4][15] == 0 && pic[10][9] == 0 && pic[10][20] == 0 && pic[15][15] == 0,
              "a square on the grid leaks outside");
        snprintf(said, sizeof said, "a 10 by 10 square covers %ld, not 100 pixels", total() / 255);
        check(total() == 100L * 255, said);
    }

    /* 2. The same square half a pixel right: its left and right columns half. */
    {
        float sq[] = { 10.5f, 5, 20.5f, 5, 20.5f, 15, 10.5f, 15 };

        gfx_path_begin(&p, 0, 0, W, H);
        gfx_path_ring(&p, sq, 4);
        read_out(&p);
        snprintf(said, sizeof said, "half a pixel in, the edges are %d and %d, not 128",
                 pic[10][10], pic[10][20]);
        check(near(pic[10][10], 128, 1) && near(pic[10][20], 128, 1) && pic[10][15] == 255, said);
        check(near(total(), 100L * 255, 20), "moved half a pixel, the square's area changed");
    }

    /* 3. Wound the other way, the same coverage: size, not sign. */
    {
        float sq[] = { 10, 5, 10, 15, 20, 15, 20, 5 };

        gfx_path_begin(&p, 0, 0, W, H);
        gfx_path_ring(&p, sq, 4);
        read_out(&p);
        check(total() == 100L * 255 && pic[10][15] == 255, "a ring wound the other way is not the same shape");
    }

    /* 4. A triangle: its coverage sums to its area, 0.5 * 20 * 20 = 200. */
    {
        float tri[] = { 5, 5, 25, 5, 5, 25 };

        gfx_path_begin(&p, 0, 0, W, H);
        gfx_path_ring(&p, tri, 3);
        read_out(&p);
        snprintf(said, sizeof said, "a triangle of area 200 covers %ld", total() / 255);
        check(near(total(), 200L * 255, 255), said);
        check(pic[6][6] == 255 && pic[20][20] == 0, "a triangle's inside or outside is wrong");
    }

    /* 5. A hole: an outer ring and an inner one wound the other way. */
    {
        float outer[] = { 5, 5, 25, 5, 25, 25, 5, 25 };
        float inner[] = { 10, 10, 10, 20, 20, 20, 20, 10 };

        gfx_path_begin(&p, 0, 0, W, H);
        gfx_path_ring(&p, outer, 4);
        gfx_path_ring(&p, inner, 4);
        read_out(&p);
        check(pic[15][15] == 0 && pic[7][7] == 255 && total() == 300L * 255,
              "a hole wound the other way did not cancel its ring");
    }

    /* 6. Two rings wound alike, overlapping: their union, never darker. */
    {
        float a[] = { 5, 5, 15, 5, 15, 15, 5, 15 };
        float b[] = { 10, 10, 20, 10, 20, 20, 10, 20 };

        gfx_path_begin(&p, 0, 0, W, H);
        gfx_path_ring(&p, a, 4);
        gfx_path_ring(&p, b, 4);
        read_out(&p);
        check(pic[12][12] == 255 && total() == 175L * 255,
              "two overlapping rings are not their union");
    }

    /* 7. A wide line: 4 wide, along y = 12, from x 5 to 30, round ends. */
    {
        float line[] = { 5, 12, 30, 12 };

        gfx_path_begin(&p, 0, 0, W, H);
        gfx_path_stroke(&p, line, 2, 4.0f);
        read_out(&p);
        check(pic[10][15] == 255 && pic[13][15] == 255 && pic[9][15] == 0 && pic[14][15] == 0,
              "a line 4 wide does not cover 4 rows");
        check(pic[12][3] > 0 && pic[12][2] == 0 && pic[12][31] > 0,
              "a line's round ends are not there");
        snprintf(said, sizeof said, "a line 25 long and 4 wide with round ends covers %ld, not about 113",
                 total() / 255);
        check(near(total() / 255, 113, 4), said);
    }

    /* 8. A joint: an L, whose corner is covered once, not twice darker. */
    {
        float l[] = { 5, 5, 20, 5, 20, 20 };

        gfx_path_begin(&p, 0, 0, W, H);
        gfx_path_stroke(&p, l, 3, 6.0f);
        read_out(&p);
        /* (21.5, 3.5) is 2.1 from the joint, inside its disc; (22.5, 2.5)
         * is 3.5 from it, where a square corner would be and a round one
         * is not. */
        snprintf(said, sizeof said, "an L's corner is not filled and rounded: %d, %d, %d",
                 pic[5][20], pic[3][21], pic[2][22]);
        check(pic[5][20] == 255 && pic[3][21] == 255 && pic[2][22] < 128, said);
    }

    /* 9. Off the left edge: covered from the edge on. Off the right: nothing. */
    {
        float left[] = { -50, 5, 5, 5, 5, 10, -50, 10 };
        float right[] = { 45, 5, 60, 5, 60, 10, 45, 10 };

        gfx_path_begin(&p, 0, 0, W, H);
        gfx_path_ring(&p, left, 4);
        gfx_path_ring(&p, right, 4);
        read_out(&p);
        check(pic[7][0] == 255 && pic[7][4] == 255 && pic[7][5] == 0 && pic[7][39] == 0,
              "a shape across the left edge does not cover from it, or one off the right shows");
    }

    /* 10. A box at an offset: (100, 200) is the box's top left. */
    {
        float sq[] = { 110, 205, 120, 205, 120, 215, 110, 215 };

        gfx_path_begin(&p, 100, 200, W, H);
        gfx_path_ring(&p, sq, 4);
        read_out(&p);
        check(pic[10][15] == 255 && total() == 100L * 255, "a box at an offset drew in the wrong place");
    }

    /* 11. Empty after every path: begun again with nothing added, nothing. */
    gfx_path_begin(&p, 0, 0, W, H);
    read_out(&p);
    check(total() == 0, "the buffer was not empty after a path was read out");

    /* 12. Above and below the box: nothing, and no write outside it. */
    {
        float tall[] = { 5, -100, 10, -100, 10, 200, 5, 200 };

        gfx_path_begin(&p, 0, 0, W, H);
        gfx_path_ring(&p, tall, 4);
        read_out(&p);
        check(pic[0][7] == 255 && pic[H - 1][7] == 255 && total() == 5L * H * 255,
              "a shape taller than the box is not cut to it");
    }

    /* 13. One buffer reused at other sizes: a small box after a large one,
     * then the large one again, each exactly its own shape. */
    {
        float small[] = { 2, 2, 8, 2, 8, 8, 2, 8 };
        float big[] = { 10, 5, 20, 5, 20, 15, 10, 15 };

        gfx_path_begin(&p, 0, 0, 12, 12);
        gfx_path_ring(&p, small, 4);
        read_out(&p);
        check(total() == 36L * 255, "a smaller box after a larger one is not its own shape");
        gfx_path_begin(&p, 0, 0, W, H);
        gfx_path_ring(&p, big, 4);
        read_out(&p);
        check(total() == 100L * 255 && pic[2][2] == 0,
              "the larger box again kept something of the smaller one");
    }

    /* 14. Two boxes side by side draw what one box does: a slanted band
     * crossing x = 20, drawn in [0, 20) and [20, 40) and then in [0, 40),
     * is the same pixels - a map's tiles meet without a seam. */
    {
        float band[] = { 3, 2, 37, 9, 36, 16, 2, 9 };
        static uint8_t whole[H][W];
        int same = 1;

        gfx_path_begin(&p, 0, 0, W, H);
        gfx_path_ring(&p, band, 4);
        read_out(&p);
        memcpy(whole, pic, sizeof pic);

        memset(pixels, 0, sizeof pixels);
        gfx_path_begin(&p, 0, 0, 20, H);
        gfx_path_ring(&p, band, 4);
        gfx_path_paint(&p, &pixels[0][0], W * 4, 0xffffff);
        gfx_path_begin(&p, 20, 0, 20, H);
        gfx_path_ring(&p, band, 4);
        gfx_path_paint(&p, &pixels[0][20], W * 4, 0xffffff);

        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int v = (int)(pixels[y][x] >> 24) - whole[y][x];

                if (v > 1 || v < -1) same = 0;
            }

        check(same, "a shape drawn in two boxes side by side differs from one box: a seam");
    }

    gfx_path_free(&p);

    if (fails == 0) {
        printf("PASS: %d checks on gfx's paths (a square on and off the grid, either winding, "
               "a triangle's area, a hole, a union, a wide line and its joint, the box's edges)\n",
               checks);
        return 0;
    }

    printf("FAIL: %d of %d checks on gfx's paths\n", fails, checks + fails);
    return 1;
}
