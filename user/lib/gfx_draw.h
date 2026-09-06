/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_GFX_DRAW_H
#define KOSMOS_GFX_DRAW_H

#include <stddef.h>
#include <stdint.h>

/*
 * What `gfx` lends to another kit that draws.
 *
 * A page is not a widget and does not go through the window manager's draw
 * ops: layout produces boxes and glyphs by the thousand, and a crossing per
 * glyph would cost more than the drawing - `gfx.md` 19.11 puts a crossing at
 * about two thousand pixels of work. So the web kit paints its own surface,
 * in C, the way `docfont.c` paints a page of a PDF.
 *
 * **The surface is opaque here on purpose.** `docfont.c` had to repeat
 * `struct surface`'s first four fields to reach the pixels, with a comment
 * admitting that two files must now agree and a static assert would be
 * better. Passing the pointer through untouched means there is nothing to
 * agree about: the caller gets it from `luaL_checkudata(L, n,
 * "kosmos.surface")`, hands it back, and only `gfx.c` ever knows the shape.
 *
 * A face is the number `gfx.face(name, px)` returns, or one of the four
 * roles - they are the same array.
 */

struct surface;

void gfx_draw_fill(struct surface *s, long x, long y, long w, long h,
                   uint32_t colour);

/* `bg` NULL leaves what is underneath, which is what text on a background
 * already painted wants. */
void gfx_draw_text(struct surface *s, int face, long x, long y,
                   const char *str, size_t len,
                   uint32_t fg, const uint32_t *bg);

long gfx_draw_measure(int face, const char *str, size_t len);
int  gfx_draw_height(int face);

/* Where the baseline sits below the top of a line. Layout needs it apart
 * from the height: two faces on one line share a baseline, not a top edge,
 * and `gfx_draw_text` takes the top. */
int  gfx_draw_ascent(int face);

#endif /* KOSMOS_GFX_DRAW_H */
