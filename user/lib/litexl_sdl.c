/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The software surface, which is the half of SDL that Lite XL draws with.
 *
 * `user/lib/litexl/SDL.h` says what this is and what it is not. In short: a
 * rectangle of pixels in ordinary memory with fill, blit and a clip
 * rectangle, written against what Kosmos has, and nothing beyond what
 * `docs/litexl.md` measured Lite XL to call.
 *
 * **Nothing here touches the framebuffer.** A surface is memory; getting one
 * onto a screen is `renwin_update_rects`, which is step three and becomes
 * the compositor's damage protocol. Keeping that line means this file can be
 * reasoned about on its own, and it is the same line `gfx.md` draws between
 * a surface and the screen.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "litexl/SDL.h"

/* ------------------------------------------------------------ allocator */

void *SDL_malloc(size_t size)             { return malloc(size); }
void *SDL_calloc(size_t n, size_t size)   { return calloc(n, size); }
void *SDL_realloc(void *p, size_t size)   { return realloc(p, size); }
void  SDL_free(void *p)                   { free(p); }

char *SDL_strdup(const char *s)
{
    size_t n;
    char  *out;

    if (s == NULL) {
        return NULL;
    }

    n   = strlen(s) + 1;
    out = malloc(n);

    if (out != NULL) {
        memcpy(out, s, n);
    }

    return out;
}

/* --------------------------------------------------------------- errors */

/*
 * One message, and it is a diagnostic rather than a mechanism.
 *
 * SDL's own is per-thread; a Kosmos process has one thread, so a single
 * buffer is exactly as correct here and says why it is allowed to be.
 */
static char last_error[128] = "";

const char *SDL_GetError(void)
{
    return last_error;
}

void SDL_SetError(const char *fmt, ...)
{
    /*
     * The format is *not* expanded, and the honest reason is that nothing
     * reads this. Lite XL prints `SDL_GetError()` beside a message it wrote
     * itself, so the useful half is already on the line; formatting here
     * would mean a `vsnprintf` in the shim to produce a string that appears
     * next to a better one.
     */
    size_t n = strlen(fmt);

    if (n >= sizeof last_error) {
        n = sizeof last_error - 1;
    }

    memcpy(last_error, fmt, n);
    last_error[n] = '\0';
}

/* -------------------------------------------------------------- formats */

static void describe(SDL_PixelFormat *f, int depth, uint32_t r, uint32_t g,
                     uint32_t b, uint32_t a)
{
    unsigned i;

    f->format        = 0;
    f->BitsPerPixel  = (uint8_t)depth;
    f->BytesPerPixel = (uint8_t)((depth + 7) / 8);
    f->Rmask = r; f->Gmask = g; f->Bmask = b; f->Amask = a;

    /*
     * The shift is where the mask's lowest set bit is, which is the whole
     * of what SDL's own is. Zero for an absent channel - a mask of zero has
     * no lowest set bit and the loop below would run off the end of the
     * word looking for one.
     */
    f->Rshift = f->Gshift = f->Bshift = f->Ashift = 0;

    for (i = 0; i < 32; i++) {
        if (r && (r >> i) & 1u) { f->Rshift = (uint8_t)i; r = 0; }
        if (g && (g >> i) & 1u) { f->Gshift = (uint8_t)i; g = 0; }
        if (b && (b >> i) & 1u) { f->Bshift = (uint8_t)i; b = 0; }
        if (a && (a >> i) & 1u) { f->Ashift = (uint8_t)i; a = 0; }
    }
}

uint32_t SDL_MapRGB(const SDL_PixelFormat *f, uint8_t r, uint8_t g, uint8_t b)
{
    /* Opaque, which is what SDL does: a format with no alpha ignores the
     * mask and one with alpha gets it full. */
    return ((uint32_t)r << f->Rshift) | ((uint32_t)g << f->Gshift)
         | ((uint32_t)b << f->Bshift) | f->Amask;
}

uint32_t SDL_MapRGBA(const SDL_PixelFormat *f, uint8_t r, uint8_t g,
                     uint8_t b, uint8_t a)
{
    return ((uint32_t)r << f->Rshift) | ((uint32_t)g << f->Gshift)
         | ((uint32_t)b << f->Bshift)
         | (f->Amask ? (((uint32_t)a << f->Ashift) & f->Amask) : 0u);
}

void SDL_GetRGBA(uint32_t pixel, const SDL_PixelFormat *f,
                 uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a)
{
    *r = (uint8_t)((pixel & f->Rmask) >> f->Rshift);
    *g = (uint8_t)((pixel & f->Gmask) >> f->Gshift);
    *b = (uint8_t)((pixel & f->Bmask) >> f->Bshift);
    *a = f->Amask ? (uint8_t)((pixel & f->Amask) >> f->Ashift) : 255;
}

/* ------------------------------------------------------------- surfaces */

static SDL_Surface *make(int w, int h, int depth, int pitch, void *pixels,
                         uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    SDL_Surface *s;

    /*
     * Refused rather than approximated.
     *
     * Lite XL asks for 32 bits and nothing else. A surface quietly handed
     * back at another depth would draw, and would draw wrong, somewhere far
     * from here - which is the class of bug this whole port is trying not
     * to have.
     */
    if (w <= 0 || h <= 0 || depth != 32) {
        SDL_SetError("only 32-bit surfaces with a positive size");
        return NULL;
    }

    s = calloc(1, sizeof *s);

    if (s == NULL) {
        SDL_SetError("out of memory");
        return NULL;
    }

    describe(&s->format_storage, depth, r, g, b, a);
    s->format = &s->format_storage;

    s->w        = w;
    s->h        = h;
    s->pitch    = (pitch > 0) ? pitch : w * s->format->BytesPerPixel;
    s->refcount = 1;

    s->clip_rect.x = 0;
    s->clip_rect.y = 0;
    s->clip_rect.w = w;
    s->clip_rect.h = h;

    if (pixels != NULL) {
        s->pixels      = pixels;
        s->owns_pixels = false;
    } else {
        s->pixels      = calloc(1, (size_t)s->pitch * (size_t)h);
        s->owns_pixels = true;

        if (s->pixels == NULL) {
            free(s);
            SDL_SetError("out of memory");
            return NULL;
        }
    }

    return s;
}

SDL_Surface *SDL_CreateRGBSurface(uint32_t flags, int width, int height,
                                  int depth, uint32_t Rmask, uint32_t Gmask,
                                  uint32_t Bmask, uint32_t Amask)
{
    (void)flags;    /* SDL2 ignores it too; it is there for SDL 1.2 */
    return make(width, height, depth, 0, NULL, Rmask, Gmask, Bmask, Amask);
}

SDL_Surface *SDL_CreateRGBSurfaceFrom(void *pixels, int width, int height,
                                      int depth, int pitch, uint32_t Rmask,
                                      uint32_t Gmask, uint32_t Bmask,
                                      uint32_t Amask)
{
    return make(width, height, depth, pitch, pixels,
                Rmask, Gmask, Bmask, Amask);
}

void SDL_FreeSurface(SDL_Surface *s)
{
    if (s == NULL) {
        return;
    }

    if (s->owns_pixels) {
        free(s->pixels);
    }

    free(s);
}

void SDL_SetClipRect(SDL_Surface *s, const SDL_Rect *rect)
{
    SDL_Rect whole;

    if (s == NULL) {
        return;
    }

    whole.x = 0; whole.y = 0; whole.w = s->w; whole.h = s->h;

    /* NULL means "the whole surface", which is how `renwin_clip_to_surface`
     * takes the clip off again. */
    if (rect == NULL) {
        s->clip_rect = whole;
        return;
    }

    if (!SDL_IntersectRect(&whole, rect, &s->clip_rect)) {
        s->clip_rect.x = s->clip_rect.y = 0;
        s->clip_rect.w = s->clip_rect.h = 0;
    }
}

void SDL_GetClipRect(SDL_Surface *s, SDL_Rect *rect)
{
    if (s != NULL && rect != NULL) {
        *rect = s->clip_rect;
    }
}

bool SDL_IntersectRect(const SDL_Rect *a, const SDL_Rect *b, SDL_Rect *out)
{
    int x0 = (a->x > b->x) ? a->x : b->x;
    int y0 = (a->y > b->y) ? a->y : b->y;
    int x1 = (a->x + a->w < b->x + b->w) ? a->x + a->w : b->x + b->w;
    int y1 = (a->y + a->h < b->y + b->h) ? a->y + a->h : b->y + b->h;

    if (x1 <= x0 || y1 <= y0) {
        return false;
    }

    out->x = x0;
    out->y = y0;
    out->w = x1 - x0;
    out->h = y1 - y0;

    return true;
}

int SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect, uint32_t colour)
{
    SDL_Rect area;
    int      y;

    if (dst == NULL) {
        return -1;
    }

    if (rect == NULL) {
        area = dst->clip_rect;
    } else if (!SDL_IntersectRect(&dst->clip_rect, rect, &area)) {
        return 0;               /* entirely clipped; not an error */
    }

    for (y = area.y; y < area.y + area.h; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)dst->pixels
                                     + (size_t)y * (size_t)dst->pitch);
        int x;

        for (x = area.x; x < area.x + area.w; x++) {
            row[x] = colour;
        }
    }

    return 0;
}

int SDL_BlitScaled(SDL_Surface *src, const SDL_Rect *srcrect,
                   SDL_Surface *dst, SDL_Rect *dstrect)
{
    SDL_Rect from, to, area;
    int      y;

    if (src == NULL || dst == NULL) {
        return -1;
    }

    if (srcrect != NULL) {
        from = *srcrect;
    } else {
        from.x = 0; from.y = 0; from.w = src->w; from.h = src->h;
    }

    if (dstrect != NULL) {
        to = *dstrect;
    } else {
        to.x = 0; to.y = 0; to.w = dst->w; to.h = dst->h;
    }

    if (to.w <= 0 || to.h <= 0 || from.w <= 0 || from.h <= 0) {
        return 0;
    }

    if (!SDL_IntersectRect(&dst->clip_rect, &to, &area)) {
        return 0;
    }

    /*
     * Nearest neighbour, and alpha-blended when the source carries alpha.
     *
     * Both halves are what the one caller needs. `ren_draw_rect` stretches a
     * *single pixel* over a rectangle to fill it, so every sample lands on
     * the same source pixel and nearest neighbour is not an approximation of
     * anything - it is exact. And that pixel carries an alpha, which is how
     * a translucent fill is drawn, so ignoring it would silently turn every
     * overlay opaque.
     */
    for (y = area.y; y < area.y + area.h; y++) {
        int       sy  = from.y + ((y - to.y) * from.h) / to.h;
        uint32_t *drow = (uint32_t *)((uint8_t *)dst->pixels
                                      + (size_t)y * (size_t)dst->pitch);
        const uint32_t *srow =
            (const uint32_t *)((const uint8_t *)src->pixels
                               + (size_t)sy * (size_t)src->pitch);
        int x;

        for (x = area.x; x < area.x + area.w; x++) {
            int      sx = from.x + ((x - to.x) * from.w) / to.w;
            uint32_t s  = srow[sx];
            unsigned a  = src->format->Amask
                          ? ((s & src->format->Amask) >> src->format->Ashift)
                          : 255u;

            if (a == 255u) {
                drow[x] = s;
                continue;
            }

            if (a == 0u) {
                continue;
            }

            {
                uint32_t d = drow[x];
                unsigned sr = (s & src->format->Rmask) >> src->format->Rshift;
                unsigned sg = (s & src->format->Gmask) >> src->format->Gshift;
                unsigned sb = (s & src->format->Bmask) >> src->format->Bshift;
                unsigned dr = (d & dst->format->Rmask) >> dst->format->Rshift;
                unsigned dg = (d & dst->format->Gmask) >> dst->format->Gshift;
                unsigned db = (d & dst->format->Bmask) >> dst->format->Bshift;

                dr = (sr * a + dr * (255u - a)) / 255u;
                dg = (sg * a + dg * (255u - a)) / 255u;
                db = (sb * a + db * (255u - a)) / 255u;

                drow[x] = (dr << dst->format->Rshift)
                        | (dg << dst->format->Gshift)
                        | (db << dst->format->Bshift)
                        | dst->format->Amask;
            }
        }
    }

    return 0;
}

/* -------------------------------------------------------------- windows */

/*
 * **There is one window and it is not this file's.**
 *
 * `doom_kosmos.c` set this pattern and its reasoning is the one that
 * matters: a port owning its own loop is an application that cannot be
 * closed, which on this desktop means a window the compositor keeps drawing
 * for ever. So the Lua side owns the window and the loop; this side is
 * handed a surface and asked to fill it.
 *
 * One because Lite XL has one. `main.c` makes a single `RenWindow` and
 * every `ren_*` call reaches it through the `window_renderer` global, so a
 * table of windows would be a table with one entry in it and a lookup that
 * cannot fail.
 */
static struct SDL_Window {
    SDL_Surface *surface;       /* a view onto the Lua side's pixels */
    void        *pixels;
    int          w, h, pitch;
    bool         shown;
} the_window;

/*
 * Damage, accumulated between frames.
 *
 * **A fixed array with an overflow flag, not a growing list.** The list has
 * one consumer, once a frame, and a compositor that is handed two hundred
 * rectangles is slower than one handed the whole window - so past a bound
 * the honest answer is "all of it" rather than an allocation. That is the
 * same trade `wm.lua` makes about damage, arrived at from the other side.
 */
#define LITEXL_DAMAGE_MAX 64

static SDL_Rect damage[LITEXL_DAMAGE_MAX];
static int      damage_count;
static bool     damage_whole;

SDL_Window *litexl_window(void)
{
    return &the_window;
}

void litexl_window_attach(void *pixels, int w, int h, int pitch)
{
    /*
     * Re-wrapped rather than copied. `SDL_CreateRGBSurfaceFrom` takes
     * memory somebody else owns, so what Lite XL's renderer writes into is
     * the window's own buffer with nothing in between - which is the whole
     * point of a `direct` window and the reason this port is worth doing.
     *
     * The old wrapper is freed and the pixels are not: `owns_pixels` is
     * false on a surface made this way, so `SDL_FreeSurface` drops the
     * bookkeeping and leaves the buffer to whoever made it.
     */
    if (the_window.surface != NULL) {
        SDL_FreeSurface(the_window.surface);
        the_window.surface = NULL;
    }

    the_window.pixels = pixels;
    the_window.w      = w;
    the_window.h      = h;
    the_window.pitch  = pitch;

    if (pixels != NULL && w > 0 && h > 0) {
        /*
         * 0xAARRGGBB, which is what a Kosmos surface is - `gfx.md` 19.1 -
         * and what Lite XL's `SDL_PIXELFORMAT_BGRA32` means on a
         * little-endian machine. The masks are the agreement between the
         * two and are written out rather than named, because a name that
         * means different things at different endiannesses is how this
         * goes wrong silently.
         */
        the_window.surface = SDL_CreateRGBSurfaceFrom(
            pixels, w, h, 32, pitch,
            0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u);
    }

    /* A new surface is entirely undrawn, so everything is damaged. */
    damage_count = 0;
    damage_whole = true;
}

SDL_Surface *SDL_GetWindowSurface(SDL_Window *window)
{
    (void)window;

    if (the_window.surface == NULL) {
        SDL_SetError("no surface attached; the Lua side has not opened a window");
    }

    return the_window.surface;
}

void SDL_UpdateWindowSurfaceRects(SDL_Window *window, const SDL_Rect *rects,
                                  int count)
{
    int i;

    (void)window;

    /*
     * **Recorded, not presented.** On a hosted SDL this would push pixels at
     * the screen; here the pixels are already in the window's own buffer,
     * so all that is left is saying which parts moved. The Lua side reads
     * them once a frame with `litexl_damage_take` and passes them to the
     * compositor, which is the one thing allowed to touch the framebuffer.
     */
    if (rects == NULL || count <= 0) {
        return;
    }

    for (i = 0; i < count; i++) {
        if (damage_count >= LITEXL_DAMAGE_MAX) {
            damage_whole = true;
            return;
        }

        damage[damage_count++] = rects[i];
    }
}

int litexl_damage_take(SDL_Rect *out, int max, bool *whole)
{
    int n = damage_count;

    if (whole != NULL) {
        *whole = damage_whole;
    }

    if (n > max) {
        n = max;

        if (whole != NULL) {
            *whole = true;      /* more than the caller can hold */
        }
    }

    if (out != NULL && n > 0) {
        memcpy(out, damage, (size_t)n * sizeof *out);
    }

    damage_count = 0;
    damage_whole = false;

    return n;
}

void SDL_GetWindowSize(SDL_Window *window, int *w, int *h)
{
    (void)window;

    if (w != NULL) { *w = the_window.w; }
    if (h != NULL) { *h = the_window.h; }
}

void SDL_ShowWindow(SDL_Window *window)
{
    /*
     * Nothing to do, and it is not a stub in the sense of being unfinished.
     * The window exists and is on screen before Lite XL is started at all -
     * the Lua side opened it - so "show" has already happened. Recorded so
     * that a later reader does not go looking for the missing half.
     */
    (void)window;
    the_window.shown = true;
}

void SDL_DestroyWindow(SDL_Window *window)
{
    (void)window;

    if (the_window.surface != NULL) {
        SDL_FreeSurface(the_window.surface);
        the_window.surface = NULL;
    }

    the_window.pixels = NULL;
    the_window.w = the_window.h = the_window.pitch = 0;
}
