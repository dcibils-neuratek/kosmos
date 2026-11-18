/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_GFX_BLUR_H
#define KOSMOS_GFX_BLUR_H

/*
 * A picture blurred: box passes, one across and one down, repeated - three
 * of them are within a few per cent of a Gaussian (`docs/astra.md`, Liquid
 * Glass LG2). What glass is frosted with: the window manager blurs the
 * wallpaper once, when it or its fit changes, and a glass surface shows the
 * part of that copy under it (wallpaper glass, G1).
 *
 * `surface:blur` (`gfx.c`) is the Lua door; this is the loop, in a file of
 * its own so the Mac can hold the vector path to the scalar one, natively
 * and through Rosetta (`tools/test_blur.c`), as `pack.c` is held.
 *
 * Every channel, alpha too, is the average of the `2r + 1` around it, the
 * edge pixel standing in past the edge, rounded by one reciprocal:
 * `(sum * m + 2^22) >> 23` with `m = round(2^23 / (2r + 1))`, which gives a
 * plain colour back exactly and fits 32 bits for every radius to
 * `GFX_BLUR_MOST` (`blur.c` says why).
 */

#include <stddef.h>
#include <stdint.h>

#define GFX_BLUR_MOST 255u      /* the largest radius of one pass */

/*
 * `src`, `w` by `h` with rows `pitch` pixels apart, blurred `passes` times
 * at radius `r` into `dst` (the same shape and pitch; it may be `src`).
 * `tmp` is `w * h` pixels with rows `w` apart, and `sums` `4 * w`: the
 * caller's, since a kit does not choose where a frame's worth of memory
 * comes from. Nothing when `r` is 0 or past `GFX_BLUR_MOST` but a copy.
 */
void gfx_blur(const uint32_t *src, uint32_t *dst, size_t pitch,
              unsigned w, unsigned h, unsigned r, unsigned passes,
              uint32_t *tmp, uint32_t *sums);

/* The same, a pixel and a channel at a time: the reference the vector
 * loops are held to, and their tails. */
void gfx_blur_scalar(const uint32_t *src, uint32_t *dst, size_t pitch,
                     unsigned w, unsigned h, unsigned r, unsigned passes,
                     uint32_t *tmp, uint32_t *sums);

/*
 * **Nebula glass** (Starlight S3, `docs/astra.md`): the blurred wallpaper
 * given its tint once, when it is made, so a frame pays nothing for it. A
 * vertical gradient - `top` on the first row to `bottom` on the last,
 * `#aarrggbb` each, its alpha how much of it covers the glass - and a
 * grain over it, the same on the three channels: each pixel moved by up to
 * `grain` levels of 255 either way, by a hash of where it is and `seed`,
 * so it is the same dust every time. Alpha is the pixel's own.
 */
#define GFX_NEBULA_GRAIN_MOST 32u

void gfx_nebula(uint32_t *pixels, size_t pitch, unsigned w, unsigned h,
                uint32_t top, uint32_t bottom, unsigned grain, uint32_t seed);

/* The same a pixel at a time: the reference and the tail. */
void gfx_nebula_scalar(uint32_t *pixels, size_t pitch, unsigned w, unsigned h,
                       uint32_t top, uint32_t bottom, unsigned grain, uint32_t seed);

#endif /* KOSMOS_GFX_BLUR_H */
