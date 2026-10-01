/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_GFX_ROWS_H
#define KOSMOS_GFX_ROWS_H

/*
 * The two loops a page's paint spends most of its pixels in, a row at a
 * time (`roadmap.md` 6zz h): filling, and laying a glyph's coverage over
 * what is there. Measured first - on Wikipedia's front page a band's fills
 * were a quarter of its paint and its text a fifth - and in a file of its
 * own, as `pack.c` is, so the Mac holds the lanes to the one-at-a-time loop
 * they replace (`tools/test_rows.c`).
 *
 * `vectors` chooses: the lanes when true, which is what `gfx.c` asks for,
 * and the scalar loop when false, which is the reference and the tail.
 */

#include <stdbool.h>
#include <stdint.h>

/* `n` pixels of `colour` from `p`. */
void gfx_fill_row(uint32_t *p, long n, uint32_t colour, bool vectors);

/*
 * A run of a glyph over a row: each of `n` pixels at `dst` gets `ink` laid
 * over it by its byte of `coverage` - 0 leaves it, 255 replaces it, and
 * anything between mixes the two, rounded, and opaque - which is `gfx.c`'s
 * `mix`, to the bit.
 */
void gfx_cover_row(uint32_t *dst, const uint8_t *coverage, long n,
                   uint32_t ink, bool vectors);

#endif /* KOSMOS_GFX_ROWS_H */
