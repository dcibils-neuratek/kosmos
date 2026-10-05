/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_GFX_DRAW_H
#define KOSMOS_GFX_DRAW_H

#include <stdbool.h>
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
 * **The surface is opaque here on purpose, and this is the one door to
 * it.** `struct surface` was once written out again in four other files -
 * the GL Kit, `png.c`, `jpeg.c` and `docfont.c` - with comments admitting
 * that the copies must agree, and the two of those that drew into one
 * checked that it had not been freed but not that a view's parent had not.
 * Now only `gfx.c` knows the shape: a kit that draws is handed the pixels by
 * `kosmos_surface_pixels`, or the surface for the calls below by
 * `gfx_surface_check`, both of which check; and a kit that decodes a
 * picture hands its pixels to `gfx_surface_new`, which makes the surface.
 *
 * A face is the number `gfx.face(name, px)` returns, or one of the four
 * roles - they are the same array.
 */

struct surface;
struct lua_State;

/*
 * The surface at `index` on the stack: its pixels, and its width, height
 * and pitch through whichever of the three are not NULL. A Lua error, not a
 * pointer, for anything that is not a live surface - one freed, or a view
 * of one that was. The pitch comes with the pointer because the pitch is
 * almost never `width * 4` (`gfx.md` 19.3).
 */
uint32_t *kosmos_surface_pixels(struct lua_State *L, int index,
                                unsigned *width, unsigned *height,
                                unsigned *pitch);

/* The same surface, held to the same checks, as the pointer the
 * `gfx_draw_*` calls below take. */
struct surface *gfx_surface_check(struct lua_State *L, int index);

/*
 * Pages for a `width` by `height` surface, zeroed, rows at the pitch every
 * surface `gfx` makes has: what `gfx.surface` asks the kernel for, for a
 * decoder that fills the pixels before the surface exists. NULL when the
 * kernel refused, with nothing to hand back. Both sides must be 1 to 16384.
 */
uint32_t *gfx_surface_map(unsigned width, unsigned height, unsigned *pitch,
                          size_t *pages);

/*
 * A surface over `pixels`, pushed onto the stack. `pages` is how many of
 * them it owns and hands back when it is freed or collected - what
 * `gfx_surface_map` said - and 0 for pixels that are somebody else's.
 */
void gfx_surface_new(struct lua_State *L, uint32_t *pixels, unsigned width,
                     unsigned height, unsigned pitch, size_t pages);

void gfx_draw_fill(struct surface *s, long x, long y, long w, long h,
                   uint32_t colour);

/* `bg` NULL leaves what is underneath, which is what text on a background
 * already painted wants. */
void gfx_draw_text(struct surface *s, int face, long x, long y,
                   const char *str, size_t len,
                   uint32_t fg, const uint32_t *bg);

/*
 * All of `src` into [dx, dx+dw) by [dy, dy+dh) of `dst`, smoothed when it is
 * scaled, over what is there by the picture's own alpha, and nothing outside
 * [cx0, cx1) by [cy0, cy1) - a page's picture at its box's size (`roadmap.md`
 * 6zz j4, j5). The same scaler as `stretch`.
 */
void gfx_draw_stretch(struct surface *dst, const struct surface *src,
                      long dx, long dy, long dw, long dh,
                      long cx0, long cy0, long cx1, long cy1);

/* How wide and tall a surface is - for a caller in C that is handed one
 * and needs to know whether drawing it at a size scales it. */
void gfx_draw_size(const struct surface *s, unsigned *width,
                   unsigned *height);

long gfx_draw_measure(int face, const char *str, size_t len);
int  gfx_draw_height(int face);

/* Where the baseline sits below the top of a line. Layout needs it apart
 * from the height: two faces on one line share a baseline, not a top edge,
 * and `gfx_draw_text` takes the top. */
int  gfx_draw_ascent(int face);

/*
 * A film's picture - three planes, 4:2:0, as a decoder hands them back -
 * onto the surface from its top left, clipped to it (the H.264 kit,
 * `roadmap.md` 4e). `bt709` and `full_range` say which of the four
 * conversions in `yuv.h` the film was made with.
 */
void gfx_draw_i420(struct surface *s, const uint8_t *const plane[3],
                   const int stride[3], unsigned width, unsigned height,
                   bool bt709, bool full_range);

/*
 * The faces off the disk a lookup from C wanted and could not have - a
 * page's text measured by NetSurf, which has no Lua state to load with -
 * loaded now (`gfx.c`, `fallback_for`). True when a face was tried - it
 * arrived, or is not on the disk and the next one will be wanted - and what
 * was measured without it should be measured again.
 */
bool gfx_fonts_load(struct lua_State *L);

#endif /* KOSMOS_GFX_DRAW_H */
