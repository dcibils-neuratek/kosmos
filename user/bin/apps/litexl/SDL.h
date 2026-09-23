/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_LITEXL_SDL_H
#define KOSMOS_LITEXL_SDL_H

/*
 * Enough of SDL2 for Lite XL, and deliberately not one function more.
 *
 * **This is not an SDL implementation and must never grow into one.** It is
 * the shim `docs/litexl.md` step two describes: the ninety-one SDL functions
 * Lite XL v2.1.7 actually calls, written against what Kosmos has, and
 * nothing beyond them. A general SDL would be a second operating system's
 * worth of surface area to maintain for one program's benefit.
 *
 * The name of the file is the whole trick. Lite XL says `#include <SDL.h>`
 * in three headers, and every one of its source files reaches SDL through
 * those - so a directory on the include path with this in it is what turns
 * "port the editor" into "write these functions". The vendored tree stays
 * byte for byte what upstream released, which is the rule
 * `runtime/upstream/lite-xl/README.kosmos.md` is built on.
 *
 * **What is here is the software surface**, which is the half of SDL that
 * Lite XL uses: a rectangle of pixels in ordinary memory, with fill, blit
 * and a clip rectangle. The other half - windows, events, the clipboard,
 * time - is `system.c`'s shim and arrives at step five. Anything declared
 * here and not defined yet says so at the link, which is the loudness this
 * wants: a port that silently returns zero from a function it never wrote
 * is a port that fails somewhere else entirely.
 *
 * There is no GPU path and there will not be one. Lite XL compiles its
 * renderer out entirely when `LITE_USE_SDL_RENDERER` is undefined, which is
 * upstream's own default, and what remains draws into a CPU buffer and
 * pushes damage rectangles - the thing this system's compositor already is.
 */

/*
 * The C headers, because the real `SDL.h` pulls them in and its callers
 * rely on that.
 *
 * `rencache.c` calls `realloc` and `rand` without including `<stdlib.h>`
 * itself - which is not sloppiness on its part, it is what SDL's own
 * `SDL_stdinc.h` promises. A shim that leaves them out makes upstream look
 * broken, and the fix would have to be a patch to a vendored file.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- types */

typedef struct SDL_Rect {
    int x, y, w, h;
} SDL_Rect;

/*
 * A colour, in the order SDL declares it.
 *
 * `renderer.c` builds one of these by hand out of a pixel and the mask and
 * shift fields below, so the member names and the order matter.
 */
typedef struct SDL_Color {
    uint8_t r, g, b, a;
} SDL_Color;

/*
 * How pixels are laid out in a surface.
 *
 * Only the fields Lite XL reads. `renderer.c` does its own blending in the
 * glyph loop rather than calling `SDL_GetRGBA` - its comment says the call
 * was a measured performance regression - so it takes the masks and shifts
 * apart itself, and they have to be real.
 */
typedef struct SDL_PixelFormat {
    uint32_t format;
    uint8_t  BitsPerPixel;
    uint8_t  BytesPerPixel;
    uint32_t Rmask, Gmask, Bmask, Amask;
    uint8_t  Rshift, Gshift, Bshift, Ashift;
} SDL_PixelFormat;

/*
 * A rectangle of pixels in ordinary memory.
 *
 * `pitch` is bytes per row and is *not* `w * BytesPerPixel` in general -
 * the same trap `CLAUDE.md` names about the framebuffer, where 1920 pixels
 * come in 7744 bytes rather than 7680. Every loop here goes through it.
 *
 * `clip_rect` is honoured by `SDL_FillRect` and `SDL_BlitScaled`, which is
 * the whole of what Lite XL uses it for: `renwin_set_clip_rect` sets it and
 * the drawing respects it.
 */
typedef struct SDL_Surface {
    uint32_t         flags;
    SDL_PixelFormat *format;
    int              w, h;
    int              pitch;
    void            *pixels;
    SDL_Rect         clip_rect;
    int              refcount;

    /* Ours: set when this surface owns `pixels` and must free them. */
    bool             owns_pixels;
    SDL_PixelFormat  format_storage;
} SDL_Surface;

/*
 * A window. Opaque here, and defined by the shim that knows what a Kosmos
 * window is - `system.c`'s half, at step five. Lite XL only ever holds a
 * pointer to one.
 */
typedef struct SDL_Window SDL_Window;

/* ------------------------------------------------------------- surfaces */

/*
 * The masks say the layout and the depth says the size, exactly as SDL's do.
 * Lite XL asks for 32-bit surfaces and nothing else; a different depth is
 * refused rather than approximated, because a surface that is not the shape
 * the caller asked for is a bug that shows up as corrupted glyphs.
 */
SDL_Surface *SDL_CreateRGBSurface(uint32_t flags, int width, int height,
                                  int depth, uint32_t Rmask, uint32_t Gmask,
                                  uint32_t Bmask, uint32_t Amask);

/* The same, over memory somebody else owns. Not freed by SDL_FreeSurface. */
SDL_Surface *SDL_CreateRGBSurfaceFrom(void *pixels, int width, int height,
                                      int depth, int pitch, uint32_t Rmask,
                                      uint32_t Gmask, uint32_t Bmask,
                                      uint32_t Amask);

void SDL_FreeSurface(SDL_Surface *surface);

/* A whole surface when `rect` is NULL, clipped to `clip_rect` either way. */
int  SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect, uint32_t colour);

/*
 * Scaled, and alpha-blended when the source has an alpha channel.
 *
 * **One caller and one shape**, which is why this is a page rather than a
 * chapter: `ren_draw_rect` makes a one-pixel surface, writes a colour into
 * it and stretches it over a rectangle, which is how Lite XL draws a
 * translucent fill. Nearest-neighbour is exact for that and honest for
 * anything else it might be handed.
 */
int  SDL_BlitScaled(SDL_Surface *src, const SDL_Rect *srcrect,
                    SDL_Surface *dst, SDL_Rect *dstrect);

void SDL_SetClipRect(SDL_Surface *surface, const SDL_Rect *rect);
void SDL_GetClipRect(SDL_Surface *surface, SDL_Rect *rect);

/* True and writes `result` when they overlap; false and leaves it when not. */
bool SDL_IntersectRect(const SDL_Rect *a, const SDL_Rect *b, SDL_Rect *result);

uint32_t SDL_MapRGB(const SDL_PixelFormat *format,
                    uint8_t r, uint8_t g, uint8_t b);
uint32_t SDL_MapRGBA(const SDL_PixelFormat *format,
                     uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void     SDL_GetRGBA(uint32_t pixel, const SDL_PixelFormat *format,
                     uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a);

/* -------------------------------------------------------------- windows */

/*
 * **A Lite XL "window" is a Kosmos surface somebody else owns.**
 *
 * `user/lib/doom_kosmos.c` set the pattern and the reasoning is its: a port
 * that owns its own loop is an application that cannot be closed, which on
 * this desktop means a window the compositor keeps drawing for ever. So the
 * Lua side owns the window and the loop, and the C side is handed a surface
 * and asked to fill it.
 *
 * That is why none of these create anything. `litexl_window_attach` is
 * called from the Lua side with the pixels of a `gfx` surface, and
 * `SDL_GetWindowSurface` hands Lite XL a view onto exactly those - so the
 * editor's renderer writes into the window's own buffer with no copy in
 * between, and `SDL_UpdateWindowSurfaceRects` records which parts changed
 * for the Lua side to pass to the compositor.
 *
 * **The editor never touches the framebuffer.** It is a `direct` window in
 * the sense `user/bin/procs.lua` defines - it owns a region the compositor
 * blits from - which is the same arrangement Doom and the cubes have.
 */
SDL_Surface *SDL_GetWindowSurface(SDL_Window *window);
void         SDL_UpdateWindowSurfaceRects(SDL_Window *window,
                                          const SDL_Rect *rects, int count);
void         SDL_GetWindowSize(SDL_Window *window, int *w, int *h);
void         SDL_ShowWindow(SDL_Window *window);
void         SDL_DestroyWindow(SDL_Window *window);

/*
 * The Kosmos side of the same object. Not SDL's, and named so.
 *
 * `attach` is called whenever the surface changes - at startup and on every
 * resize - and re-wraps it. `damage_take` hands the accumulated rectangles
 * to the caller and empties the list, which is what the Lua side does once
 * a frame before telling the compositor.
 */
/*
 * A font's bytes, handed over by the Lua side.
 *
 * `ren_font_load` takes a *filename*, and there is no `fopen` here - a path
 * means nothing without a namespace, and C down here has none. So the Lua
 * side reads the file and calls this, and `ren_font_load` looks the name up
 * among what it was given. `doom_kosmos.c` met the same wall with the WAD
 * and answered it the same way.
 *
 * The bytes are borrowed, not copied: `stb_truetype` reads them for as long
 * as the face exists, so whoever provides them keeps them alive.
 */
void litexl_font_provide(const char *path, const void *bytes, size_t len);

/*
 * The two parts of `system` that are computation rather than a question for
 * a server, separated from their Lua bindings so they can be tested without
 * a `lua_State` - which is what `tools/test_litexl_surface.c` does.
 *
 * `fuzzy_match` runs over every file in the project on every keystroke of
 * the command palette, and `path_before` orders every listing. Both are the
 * shape `CLAUDE.md` sends to C: a loop over bytes on a path somebody waits
 * on.
 */
bool litexl_fuzzy_match(const char *hay, const char *needle, bool file,
                        int *score_out);
bool litexl_path_before(const char *a, bool a_dir,
                        const char *b, bool b_dir);

SDL_Window *litexl_window(void);
void        litexl_window_attach(void *pixels, int w, int h, int pitch);
void        litexl_window_swap(void *pixels);
int         litexl_damage_take(SDL_Rect *out, int max, bool *whole);

/* --------------------------------------------------------------- errors */

const char *SDL_GetError(void);
void        SDL_SetError(const char *fmt, ...);

/* ------------------------------------------------------------ allocator */

/*
 * SDL's allocator, which Lite XL uses directly in forty-five places. It is
 * the libc one; SDL's exists so that an application can replace it, and
 * nothing here wants to.
 */
void *SDL_malloc(size_t size);
void *SDL_calloc(size_t n, size_t size);
void *SDL_realloc(void *p, size_t size);
void  SDL_free(void *p);
char *SDL_strdup(const char *s);

#endif /* KOSMOS_LITEXL_SDL_H */
