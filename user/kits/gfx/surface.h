/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_GFX_SURFACE_H
#define KOSMOS_GFX_SURFACE_H

/*
 * **A surface, as the drawing code sees it** - its shape, and the few
 * helpers every loop over its pixels uses, inline (Astra's D3b-1,
 * `docs/astra-display.md`).
 *
 * `gfx_draw.h` keeps the shape from other kits on purpose, and this does
 * not undo that: it is for `gfx.c`, `draw.c` and `scene_core.c` - the kit,
 * its primitives and the scene - and for the window server, which composes
 * with the same code and has no Lua. Everything else still goes through
 * `gfx_draw.h`.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct surface {
    uint32_t *pixels;       /* NULL once freed; every method checks */
    unsigned  width;
    unsigned  height;
    unsigned  pitch;        /* bytes per row, never assume width * 4 */
    size_t    pages;        /* what to hand back; 0 for the screen */
    bool      owned;        /* false for the screen: not ours to free */
};

/* The start of a row. The one place row arithmetic happens. */
static inline uint32_t *row_of(const struct surface *s, unsigned y)
{
    return (uint32_t *)((uint8_t *)s->pixels + (size_t)y * s->pitch);
}

/*
 * Clips a rectangle to a surface, in place.
 *
 * Clipping rather than raising, because a window half off the edge of the
 * screen is the normal case and not an error. Returns false when nothing is
 * left, so callers can return without a special case for an empty rectangle.
 *
 * The coordinates arrive as signed, because a negative origin is exactly
 * what a partly-off-screen blit looks like, and the clip has to move the
 * source origin by the same amount it moves the destination.
 */
static inline bool clip(const struct surface *s, long *x, long *y, long *w, long *h,
                 long *sx, long *sy)
{
    if (*w <= 0 || *h <= 0) {
        return false;
    }

    if (*x < 0) {
        *w += *x;
        if (sx != NULL) {
            *sx -= *x;
        }
        *x = 0;
    }

    if (*y < 0) {
        *h += *y;
        if (sy != NULL) {
            *sy -= *y;
        }
        *y = 0;
    }

    if (*x + *w > (long)s->width) {
        *w = (long)s->width - *x;
    }

    if (*y + *h > (long)s->height) {
        *h = (long)s->height - *y;
    }

    return *w > 0 && *h > 0;
}

/*
 * (x * a) / 255, rounded, without a divide.
 *
 * The standard identity, and it is exact for every pair in 0..255 rather
 * than close: there is a test that checks all 65,536 of them against the
 * rounded quotient, because "exact" is the kind of claim that is repeated
 * from memory and occasionally wrong.
 */
static inline uint32_t mul255(uint32_t x, uint32_t a)
{
    uint32_t t = x * a + 128u;
    return (t + (t >> 8)) >> 8;
}

/* Source-over, with the source's own alpha scaled by a global one. */
static inline uint32_t over(uint32_t src, uint32_t dst, uint32_t global)
{
    uint32_t a = mul255((src >> 24) & 0xffu, global);
    uint32_t inv;

    if (a == 0) {
        return dst;
    }

    if (a == 255u) {
        return (src & 0x00ffffffu) | 0xff000000u;
    }

    inv = 255u - a;

    /*
     * **The destination's own alpha, which this used to assume was 255.**
     *
     * It ended `return 0xff000000u | ...`, and for a destination that is
     * opaque - a window's backbuffer, the screen - that is right and this
     * arithmetic is unchanged for it: `da` is 255, the weight below becomes
     * `inv`, and the result is opaque.
     *
     * The desktop is not that. It is a *transparent* surface with icons
     * drawn into it and blended over the wallpaper afterwards, so a
     * shadow pixel at 40% was composited against transparent black -
     * darkening it toward nothing - and then stamped fully opaque. Haiku's
     * soft drop shadows came out as hard black fringes, which is what a
     * person sees and calls a border round the icon.
     *
     * Source-over on straight alpha, written out: the result covers what
     * the source covers plus what the destination still shows through it,
     * and each contributes in proportion. The divide is the price of a
     * destination that is not opaque, and it is paid only on this path -
     * a fully transparent or fully opaque source has already returned.
     */
    {
        uint32_t da = (dst >> 24) & 0xffu;
        uint32_t wd = mul255(da, inv);
        uint32_t oa = a + wd;
        uint32_t r, g, b;

        if (oa == 0) {
            return 0;                   /* nothing covers anything */
        }

        r = (mul255((src >> 16) & 0xffu, a)
             + mul255((dst >> 16) & 0xffu, wd)) * 255u / oa;
        g = (mul255((src >>  8) & 0xffu, a)
             + mul255((dst >>  8) & 0xffu, wd)) * 255u / oa;
        b = (mul255((src      ) & 0xffu, a)
             + mul255((dst      ) & 0xffu, wd)) * 255u / oa;

        if (r > 255u) { r = 255u; }
        if (g > 255u) { g = 255u; }
        if (b > 255u) { b = 255u; }

        return (oa << 24) | (r << 16) | (g << 8) | b;
    }
}

typedef uint32_t u32x4 __attribute__((vector_size(16)));

static inline u32x4 mul255v(u32x4 x, u32x4 a)
{
    u32x4 t = x * a + 128u;
    return (t + (t >> 8)) >> 8;
}

/*
 * **Only for an opaque destination**, which is why `blend_row` checks before
 * calling it. `over` above has to compute the result's alpha because the
 * desktop is a transparent surface; here the destination covers everything
 * already, so the output is opaque and the weights collapse to `a` and
 * `255 - a` with no divide. Keeping the divide out of the vector path is
 * most of why this is eight times quicker.
 */
static inline u32x4 over4(u32x4 src, u32x4 dst, u32x4 global)
{
    u32x4 a   = mul255v((src >> 24) & 0xffu, global);
    u32x4 inv = 255u - a;

    u32x4 r = mul255v((src >> 16) & 0xffu, a) + mul255v((dst >> 16) & 0xffu, inv);
    u32x4 g = mul255v((src >>  8) & 0xffu, a) + mul255v((dst >>  8) & 0xffu, inv);
    u32x4 b = mul255v((src      ) & 0xffu, a) + mul255v((dst      ) & 0xffu, inv);

    return 0xff000000u | (r << 16) | (g << 8) | b;
}

/*
 * One pixel of a rounded shape: `cover` is how much of the pixel the shape
 * takes (0..255), and the colour's own alpha says how opaque the shape is.
 *
 * **Both, multiplied.** The round primitives used to take only the
 * coverage and write the colour's alpha byte straight through where it was
 * whole, which is right for every opaque colour and wrong for any other: a
 * shadow under a switch's knob, `0x38000000`, came out solid black. A
 * translucent round fill is a thing the kit wants - a knob's shadow, a
 * selection laid over a picture - so the primitive does it rather than each
 * caller faking it with a grey.
 */
static inline void round_put(uint32_t *p, uint32_t colour, long cover)
{
    long a = (cover * (long)(colour >> 24) + 127) / 255;

    if (a >= 255) {
        *p = colour | 0xff000000u;
    } else if (a > 0) {
        *p = over((colour & 0x00ffffffu) | ((uint32_t)a << 24), *p, 255);
    }
}

#endif
