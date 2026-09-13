/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Super Nintendo's picture into a surface, at scale 1 and 2, on the host.
 *
 * No core and no ROM. `snes_blit` is the only code between the picture the
 * core draws and the window, so it is tested alone, with a picture small
 * enough to reason about - four by three, every pixel a different colour and
 * none of them opaque - and surfaces shaped the awkward ways real ones are:
 * rows padded wider than the pixels, and a window smaller than the picture.
 *
 * Every byte the blit must not touch is a canary, and every canary is
 * checked, because a copy that is right where it writes and writes one row
 * too many is exactly the bug this exists to catch.
 */

#include <stdint.h>
#include <stdio.h>

#include "snes_blit.h"

#define SW      4u
#define SH      3u
#define CANARY  0x5a5a5a5au
#define ROOM    256u

static unsigned checks, fails;

static void check(int ok, const char *what)
{
    checks++;

    if (!ok) {
        fails++;
        printf("FAIL: %s\n", what);
    }
}

static uint32_t src[SW * SH];
static uint32_t dst[ROOM];

static void fill(void)
{
    unsigned i;

    for (i = 0; i < ROOM; i++) {
        dst[i] = CANARY;
    }
}

/* What a pixel of a `w` by `h` window at `scale` must hold, row width `stride`
 * pixels, or CANARY outside it. */
static int holds(unsigned w, unsigned h, unsigned stride, unsigned scale)
{
    unsigned x, y;

    for (y = 0; y < ROOM / stride; y++) {
        for (x = 0; x < stride; x++) {
            uint32_t got = dst[y * stride + x];
            int inside = x < w && y < h && x / scale < SW && y / scale < SH;
            uint32_t want = inside
                ? (src[(y / scale) * SW + x / scale] | 0xff000000u) : CANARY;

            if (got != want) {
                return 0;
            }
        }
    }

    return 1;
}

int main(void)
{
    unsigned i;

    for (i = 0; i < SW * SH; i++) {
        src[i] = (i + 1u) * 0x00010203u;       /* distinct, alpha zero */
    }

    /* Scale 1, a surface exactly the picture's size. */
    fill();
    snes_blit(dst, SW, SH, SW * 4u, src, SW, SH, 1);
    check(holds(SW, SH, SW, 1), "scale 1 copies the picture, opaque, and nothing past it");

    /* Scale 0 is scale 1. */
    fill();
    snes_blit(dst, SW, SH, SW * 4u, src, SW, SH, 0);
    check(holds(SW, SH, SW, 1), "scale 0 behaves as scale 1");

    /* Scale 2, rows padded four pixels wider than the window. */
    fill();
    snes_blit(dst, SW * 2u, SH * 2u, (SW * 2u + 4u) * 4u, src, SW, SH, 2);
    check(holds(SW * 2u, SH * 2u, SW * 2u + 4u, 2),
          "scale 2 makes every pixel a two-by-two block, and leaves the padding alone");

    /* Scale 2 into a window smaller than the doubled picture, both ways. */
    fill();
    snes_blit(dst, 5u, 3u, 8u * 4u, src, SW, SH, 2);
    check(holds(5u, 3u, 8u, 2),
          "scale 2 into a smaller window writes only what fits");

    /* A pitch too narrow for a row of the window: nothing written. */
    fill();
    snes_blit(dst, 8u, 6u, 7u * 4u, src, SW, SH, 2);
    check(holds(0u, 0u, 8u, 1), "a pitch narrower than a row writes nothing");

    if (fails != 0) {
        printf("FAIL: %u of %u checks on the Super Nintendo's picture at a scale\n",
               fails, checks);
        return 1;
    }

    printf("PASS: %u checks on the Super Nintendo's picture at a scale, on this "
           "machine (scale 1 and 2, padded rows, a window smaller than the "
           "picture, and every byte it must not touch).\n", checks);
    return 0;
}
