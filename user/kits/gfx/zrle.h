/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_GFX_ZRLE_H
#define KOSMOS_GFX_ZRLE_H

/*
 * A rectangle of a surface as VNC's ZRLE tiles, before the zlib stream
 * (RFC 6143, 7.7.6; `roadmap.md`, remote 7c).
 *
 * `surface:zrle` (`gfx.c`) is the Lua door; this is the loop, in a file of
 * its own so the Mac can hold it to a decoder of its own (`tools/test_zrle.c`),
 * as `pack.c` is held. The deflating is the Compression Kit's
 * (`compress.zstream`), one stream a viewer: ZRLE's zlib stream runs for the
 * whole connection, so it cannot be a step of this.
 *
 * Each tile of 64 by 64 - smaller at the right and bottom edges - is written
 * the smallest of five ways: one colour; a palette of 2 to 16 with each
 * pixel's index packed into 1, 2 or 4 bits; runs of a palette of up to 127;
 * runs of pixels; or the pixels as they are. A desktop is flat colour and
 * text, so most tiles are the first or a small palette, and a byte or two
 * stands for thousands.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pack.h"

#define GFX_ZRLE_TILE 64

/*
 * How many bytes a CPIXEL is in this format - ZRLE's compressed pixel: the
 * format's own size, except a 32-bit pixel of depth 24 or less whose colour
 * fits in its low or its high three bytes, which drops the fourth.
 */
unsigned gfx_zrle_cpixel(const struct gfx_pack_format *f, unsigned depth);

/* The most a `w` by `h` rectangle can come to, whatever is in it. */
size_t gfx_zrle_bound(unsigned w, unsigned h);

/*
 * The rectangle whose top-left pixel is `src`, `pitch` pixels a row apart,
 * as ZRLE's tiles into `out`. Returns the bytes written, or 0 when `cap` is
 * less than `gfx_zrle_bound` - checked before anything is written.
 */
size_t gfx_zrle_rect(const uint32_t *src, size_t pitch, unsigned w, unsigned h,
                     const struct gfx_pack_format *f, unsigned depth,
                     uint8_t *out, size_t cap);

#endif /* KOSMOS_GFX_ZRLE_H */
