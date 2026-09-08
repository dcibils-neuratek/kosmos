/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Lite XL's renderer, on `stb_truetype` instead of FreeType.
 *
 * **This is the one file of the port that replaces upstream rather than
 * shimming under it**, and the reason is specific rather than a preference.
 *
 * `runtime/upstream/lite-xl/src/renderer.c` does not merely *call*
 * FreeType - it edits glyph outlines before rasterising them.
 * `FT_Outline_Translate` for subpixel positioning, `FT_Outline_Embolden`
 * for synthetic bold, `FT_Outline_Transform` with a shear matrix for
 * synthetic italic. `stb_truetype` rasterises straight from the font's
 * glyph data and has no editable outline to hand back, so a `ft2build.h`
 * shim of the kind `SDL.h` is would mean *implementing a font engine*, not
 * adapting one. That is a different project.
 *
 * So the build leaves `renderer.c` out and this provides `renderer.h`'s
 * `ren_*` interface instead. **The vendored tree is still byte for byte
 * what upstream released** - nothing is patched, one more file simply is
 * not compiled, exactly as `api/process.c` and `api/dirmonitor/` are not.
 * `LICENSE` draws the Doom line in the build for the same reason.
 *
 *--------------------------------------------------------------------------
 * What is given up, and it should be read before the code
 *
 * **Subpixel (LCD) antialiasing.** Lite XL asks for it with
 * `FONT_ANTIALIASING_SUBPIXEL` and FreeType answers with three coverage
 * values per pixel. `stb_truetype` produces one. The flag is accepted and
 * rendered as grayscale, which is what `FONT_ANTIALIASING_GRAYSCALE` would
 * have given - text that is slightly softer on an LCD and identical
 * everywhere else.
 *
 * **Synthetic bold and italic.** Those are the outline transforms above. A
 * bold face here has to be a bold *font file*, which is how
 * `assets/fonts/` is organised anyway - IBM Plex ships Regular, Bold,
 * Italic and BoldItalic as four files. Asking for `FONT_STYLE_BOLD` on a
 * regular face gets the regular face and does not pretend otherwise.
 *
 * **Hinting.** `stb_truetype` does not hint. On the framebuffer this
 * machine has, at the sizes an editor uses, that is a difference somebody
 * would have to be told about to notice.
 *
 *--------------------------------------------------------------------------
 * Where a font comes from
 *
 * Not from `fopen`, which in this system is a function that fails and
 * deliberately: there is no global tree, so there is no path to open.
 * `doom_kosmos.c` met this first with the WAD and its answer is this one -
 * **the Lua side reads the file through the namespace and hands the bytes
 * over**, and `ren_font_load` looks the name up among what it was given.
 *
 * That is why `litexl_font_provide` exists and why it is not a hack: a
 * path is only meaningful inside a namespace, and C down here has none.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "litexl/SDL.h"
#include "renderer.h"
#include "renwindow.h"

/*
 * The implementation lives in `runtime/upstream/stb/stb_impl.c` and is
 * compiled once for the whole userland - `docfont.c` reaches it the same
 * way. Declaring it static here would give this file its own private copy
 * of a font rasteriser, which is the sort of thing that shows up as a
 * mysterious hundred kilobytes.
 */
#include "stb_truetype.h"

/*--------------------------------------------------------------------------
 * The fonts the Lua side has handed over.
 *
 * A small fixed table, because the number is small and known: Lite XL loads
 * the two or three faces named in its `style.lua` and one icon font. A list
 * that grew would be a list that could leak.
 *------------------------------------------------------------------------*/

#define LITEXL_FONTS_MAX 8

static struct provided {
    char                 path[128];
    const unsigned char *bytes;
    size_t               len;
} provided[LITEXL_FONTS_MAX];

static int provided_count;

void litexl_font_provide(const char *path, const void *bytes, size_t len)
{
    struct provided *p;

    if (path == NULL || bytes == NULL || provided_count >= LITEXL_FONTS_MAX) {
        return;
    }

    p = &provided[provided_count++];

    strncpy(p->path, path, sizeof p->path - 1);
    p->path[sizeof p->path - 1] = '\0';
    p->bytes = bytes;
    p->len   = len;
}

/*
 * Matched on the last path component rather than the whole path.
 *
 * Lite XL builds font paths out of `DATADIR`, which is a notion of an
 * installed tree that does not exist here - it will ask for something like
 * `/lite-xl/fonts/JetBrainsMono-Regular.ttf`. What the Lua side handed over
 * is the file it actually read. The name is the part both agree on.
 */
static const struct provided *find_font(const char *path)
{
    const char *want = path;
    const char *slash;
    int         i;

    if (path == NULL) {
        return NULL;
    }

    for (slash = path; *slash; slash++) {
        if (*slash == '/' || *slash == '\\') {
            want = slash + 1;
        }
    }

    for (i = 0; i < provided_count; i++) {
        const char *have  = provided[i].path;
        const char *hslash;

        for (hslash = provided[i].path; *hslash; hslash++) {
            if (*hslash == '/' || *hslash == '\\') {
                have = hslash + 1;
            }
        }

        if (strcmp(have, want) == 0) {
            return &provided[i];
        }
    }

    return NULL;
}

/*--------------------------------------------------------------------------
 * A face, and its glyphs
 *------------------------------------------------------------------------*/

/*
 * **A glyph is rasterised once**, which is `docfont.c`'s first sentence and
 * is the whole reason text on this machine is affordable. An editor redraws
 * the same few hundred glyphs every frame; rasterising an outline is
 * hundreds of times what blitting a cached coverage map costs.
 *
 * Direct-mapped on the codepoint rather than a hash. A collision costs one
 * re-rasterisation and no correctness, the table is a fixed size, and the
 * alternative is a structure with a failure mode.
 */
#define GLYPH_SLOTS 512

struct cached {
    int            codepoint;   /* -1 when the slot is empty */
    unsigned char *coverage;    /* w * h bytes, 0..255 */
    int            w, h;
    int            left, top;   /* where it sits relative to the pen */
    int            advance;     /* whole pixels; see the note in draw */
    bool           valid;
};

struct RenFont {
    stbtt_fontinfo info;
    const unsigned char *bytes;
    size_t         len;
    char           path[128];

    float          size;        /* in pixels, as Lite XL counts */
    float          scale;       /* stbtt units -> pixels */
    int            ascent, descent, linegap;
    int            height;      /* what `ren_font_group_get_height` answers */
    int            baseline;    /* pixels from the top of the line box */
    int            tab_size;
    unsigned char  style;
    ERenFontAntialiasing antialiasing;
    ERenFontHinting      hinting;

    struct cached  cache[GLYPH_SLOTS];
};

RenWindow window_renderer = { 0 };

static void cache_clear(RenFont *f)
{
    int i;

    for (i = 0; i < GLYPH_SLOTS; i++) {
        free(f->cache[i].coverage);
        f->cache[i].coverage = NULL;
        f->cache[i].codepoint = -1;
        f->cache[i].valid = false;
    }
}

static void measure(RenFont *f)
{
    f->scale = stbtt_ScaleForPixelHeight(&f->info, f->size);

    stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->linegap);

    /*
     * The line height Lite XL lays out with.
     *
     * Ascent minus descent, and *not* plus the line gap: the editor adds
     * its own spacing in `style.lua` and a gap counted twice reads as
     * double-spaced text, which is what it looked like the first time.
     */
    f->height = (int)((f->ascent - f->descent) * f->scale + 0.5f);

    if (f->height < 1) {
        f->height = 1;
    }

    /*
     * Where the baseline sits inside the line box.
     *
     * Upstream computes a glyph's row as `y + baseline - bitmap_top + line`,
     * with `y` the top of the box and FreeType's `bitmap_top` measured up
     * from the baseline. `stb_truetype` hands back the same offset already
     * pointing down - its `yoff` is from the baseline to the bitmap's top
     * row - so the same sum here is `y + baseline + top + row`, with no
     * sign to get wrong.
     */
    f->baseline = (int)(f->ascent * f->scale + 0.5f);
}

static struct cached *glyph_of(RenFont *f, int codepoint)
{
    struct cached *c = &f->cache[(unsigned)codepoint % GLYPH_SLOTS];
    int            gw, gh, gx, gy, adv, lsb;
    unsigned char *bitmap;

    if (c->valid && c->codepoint == codepoint) {
        return c;
    }

    free(c->coverage);
    memset(c, 0, sizeof *c);
    c->codepoint = codepoint;
    c->valid     = true;

    stbtt_GetCodepointHMetrics(&f->info, codepoint, &adv, &lsb);
    c->advance = (int)(adv * f->scale + 0.5f);

    bitmap = stbtt_GetCodepointBitmap(&f->info, f->scale, f->scale,
                                      codepoint, &gw, &gh, &gx, &gy);

    if (bitmap == NULL) {
        /* A blank - a space, or a glyph this face does not have. The
         * advance is still right and is the whole of what a space is. */
        return c;
    }

    c->coverage = malloc((size_t)gw * (size_t)gh);

    if (c->coverage == NULL) {
        stbtt_FreeBitmap(bitmap, NULL);
        return c;
    }

    memcpy(c->coverage, bitmap, (size_t)gw * (size_t)gh);
    stbtt_FreeBitmap(bitmap, NULL);

    c->w    = gw;
    c->h    = gh;
    c->left = gx;
    c->top  = gy;

    return c;
}

/*--------------------------------------------------------------------------
 * UTF-8, because that is what Lite XL hands over
 *------------------------------------------------------------------------*/

static int decode(const char *s, size_t len, size_t *i)
{
    unsigned char b = (unsigned char)s[*i];
    int           cp;
    int           extra;

    if (b < 0x80u)        { (*i)++; return b; }
    else if (b < 0xE0u)   { cp = b & 0x1F; extra = 1; }
    else if (b < 0xF0u)   { cp = b & 0x0F; extra = 2; }
    else                  { cp = b & 0x07; extra = 3; }

    if (*i + (size_t)extra >= len + 1u && *i + (size_t)extra > len - 1u) {
        (*i)++;
        return 0xFFFD;      /* truncated; the replacement character */
    }

    (*i)++;

    while (extra-- > 0 && *i < len) {
        cp = (cp << 6) | ((unsigned char)s[*i] & 0x3F);
        (*i)++;
    }

    return cp;
}

/*--------------------------------------------------------------------------
 * The font group
 *
 * Lite XL passes `RenFont **` everywhere: an array of up to
 * FONT_FALLBACK_MAX faces, the first being the one asked for and the rest
 * fallbacks for glyphs it does not have. Unused entries are NULL.
 *------------------------------------------------------------------------*/

static RenFont *first(RenFont **group)
{
    int i;

    if (group == NULL) {
        return NULL;
    }

    for (i = 0; i < FONT_FALLBACK_MAX; i++) {
        if (group[i] != NULL) {
            return group[i];
        }
    }

    return NULL;
}

/* The first face in the group that has this codepoint, or the first face. */
static RenFont *face_for(RenFont **group, int codepoint)
{
    int i;

    for (i = 0; i < FONT_FALLBACK_MAX && group[i] != NULL; i++) {
        if (stbtt_FindGlyphIndex(&group[i]->info, codepoint) != 0) {
            return group[i];
        }
    }

    return first(group);
}

/*--------------------------------------------------------------------------
 * renderer.h
 *------------------------------------------------------------------------*/

RenFont *ren_font_load(RenWindow *window, const char *filename, float size,
                       ERenFontAntialiasing antialiasing,
                       ERenFontHinting hinting, unsigned char style)
{
    const struct provided *src = find_font(filename);
    RenFont               *f;
    int                    offset;

    (void)window;

    if (src == NULL) {
        return NULL;            /* the Lua side never handed this one over */
    }

    f = calloc(1, sizeof *f);

    if (f == NULL) {
        return NULL;
    }

    f->bytes = src->bytes;
    f->len   = src->len;
    strncpy(f->path, filename, sizeof f->path - 1);

    offset = stbtt_GetFontOffsetForIndex(f->bytes, 0);

    if (offset < 0 || !stbtt_InitFont(&f->info, f->bytes, offset)) {
        free(f);
        return NULL;
    }

    f->size         = (size > 1.0f) ? size : 1.0f;
    f->tab_size     = 4;
    f->style        = style;
    f->antialiasing = antialiasing;
    f->hinting      = hinting;

    cache_clear(f);
    measure(f);

    return f;
}

RenFont *ren_font_copy(RenWindow *window, RenFont *font, float size,
                       ERenFontAntialiasing antialiasing,
                       ERenFontHinting hinting, int style)
{
    if (font == NULL) {
        return NULL;
    }

    return ren_font_load(window, font->path, size,
                         (antialiasing < 0) ? font->antialiasing : antialiasing,
                         (hinting < 0) ? font->hinting : hinting,
                         (style < 0) ? font->style : (unsigned char)style);
}

const char *ren_font_get_path(RenFont *font)
{
    return (font != NULL) ? font->path : "";
}

void ren_font_free(RenFont *font)
{
    if (font == NULL) {
        return;
    }

    cache_clear(font);
    free(font);
}

int ren_font_group_get_tab_size(RenFont **group)
{
    RenFont *f = first(group);

    return (f != NULL) ? f->tab_size : 4;
}

void ren_font_group_set_tab_size(RenFont **group, int n)
{
    int i;

    for (i = 0; i < FONT_FALLBACK_MAX && group[i] != NULL; i++) {
        group[i]->tab_size = n;
    }
}

int ren_font_group_get_height(RenFont **group)
{
    RenFont *f = first(group);

    return (f != NULL) ? f->height : 1;
}

float ren_font_group_get_size(RenFont **group)
{
    RenFont *f = first(group);

    return (f != NULL) ? f->size : 1.0f;
}

void ren_font_group_set_size(RenWindow *window, RenFont **group, float size)
{
    int i;

    (void)window;

    for (i = 0; i < FONT_FALLBACK_MAX && group[i] != NULL; i++) {
        group[i]->size = (size > 1.0f) ? size : 1.0f;
        cache_clear(group[i]);
        measure(group[i]);
    }
}

double ren_font_group_get_width(RenWindow *window, RenFont **group,
                                const char *text, size_t len, int *x_offset)
{
    double width = 0;
    size_t i     = 0;

    (void)window;

    if (first(group) == NULL) {
        return 0;
    }

    while (i < len) {
        int      cp = decode(text, len, &i);
        RenFont *f  = face_for(group, cp);

        width += glyph_of(f, cp)->advance;
    }

    /*
     * Where the first glyph's ink starts relative to the pen. Lite XL uses
     * it to place a caret exactly; zero is right for every face here
     * because the advance already includes the bearing.
     */
    if (x_offset != NULL) {
        *x_offset = 0;
    }

    return width;
}

double ren_draw_text(RenSurface *rs, RenFont **group, const char *text,
                     size_t len, float x, int y, RenColor colour)
{
    SDL_Surface *s     = (rs != NULL) ? rs->surface : NULL;
    RenFont     *lead  = first(group);
    SDL_Rect     clip;
    double       pen   = x;
    size_t       i     = 0;
    int          clip_x1, clip_y1;

    if (s == NULL || lead == NULL) {
        return x;
    }

    SDL_GetClipRect(s, &clip);
    clip_x1 = clip.x + clip.w;
    clip_y1 = clip.y + clip.h;

    while (i < len) {
        int            cp = decode(text, len, &i);
        RenFont       *f  = face_for(group, cp);
        struct cached *g  = glyph_of(f, cp);
        int            gx = (int)(pen) + g->left;
        int            gy = y + f->baseline + g->top;
        int            row;

        if (g->coverage == NULL || colour.a == 0) {
            pen += g->advance;      /* a space, or nothing to draw */
            continue;
        }

        /* Entirely off one side: skip it without touching a pixel. */
        if (gx >= clip_x1 || gx + g->w <= clip.x
            || gy >= clip_y1 || gy + g->h <= clip.y) {
            pen += g->advance;
            continue;
        }

        for (row = 0; row < g->h; row++) {
            int       ty = gy + row;
            uint32_t *dst;
            int       col;

            if (ty < clip.y) { continue; }
            if (ty >= clip_y1) { break; }

            dst = (uint32_t *)((uint8_t *)s->pixels
                               + (size_t)ty * (size_t)s->pitch);

            for (col = 0; col < g->w; col++) {
                int      tx = gx + col;
                unsigned a  = g->coverage[(size_t)row * (size_t)g->w
                                          + (size_t)col];
                uint32_t d;
                unsigned dr, dg, db;

                if (tx < clip.x || tx >= clip_x1 || a == 0) {
                    continue;
                }

                /*
                 * Coverage times the text's own alpha, which is what lets
                 * the editor dim a whole run - a comment, a selection
                 * behind the caret - without a second pass over it.
                 */
                if (colour.a != 255) {
                    a = (a * (unsigned)colour.a) / 255u;

                    if (a == 0) {
                        continue;
                    }
                }

                if (a == 255u) {
                    dst[tx] = 0xFF000000u | ((uint32_t)colour.r << 16)
                            | ((uint32_t)colour.g << 8) | colour.b;
                    continue;
                }

                d  = dst[tx];
                dr = (d >> 16) & 0xFFu;
                dg = (d >> 8)  & 0xFFu;
                db =  d        & 0xFFu;

                dr = (colour.r * a + dr * (255u - a)) / 255u;
                dg = (colour.g * a + dg * (255u - a)) / 255u;
                db = (colour.b * a + db * (255u - a)) / 255u;

                dst[tx] = 0xFF000000u | (dr << 16) | (dg << 8) | db;
            }
        }

        pen += g->advance;
    }

    /*
     * Underline and strikethrough, which are the font's *style* rather than
     * anything in the glyphs, and are drawn as what they are: a line.
     */
    if (lead->style & FONT_STYLE_UNDERLINE) {
        ren_draw_rect(rs, (RenRect){ (int)x, y + lead->height - 2,
                                     (int)(pen - x), 1 }, colour);
    }

    if (lead->style & FONT_STYLE_STRIKETHROUGH) {
        ren_draw_rect(rs, (RenRect){ (int)x, y + lead->height / 2,
                                     (int)(pen - x), 1 }, colour);
    }

    return pen;
}

void ren_draw_rect(RenSurface *rs, RenRect rect, RenColor colour)
{
    SDL_Surface *s = (rs != NULL) ? rs->surface : NULL;
    SDL_Rect     dst;

    if (s == NULL || rect.width <= 0 || rect.height <= 0) {
        return;
    }

    dst.x = rect.x;
    dst.y = rect.y;
    dst.w = rect.width;
    dst.h = rect.height;

    if (colour.a == 255) {
        SDL_FillRect(s, &dst, SDL_MapRGB(s->format, colour.r, colour.g,
                                         colour.b));
        return;
    }

    /*
     * Translucent, which upstream does by stretching a one-pixel surface.
     * The shim's `SDL_BlitScaled` is written for exactly that shape and
     * blends, so this is the same call for the same reason.
     */
    {
        static SDL_Surface *one;

        if (one == NULL) {
            one = SDL_CreateRGBSurface(0, 1, 1, 32, 0x00FF0000u, 0x0000FF00u,
                                       0x000000FFu, 0xFF000000u);
        }

        if (one != NULL) {
            *(uint32_t *)one->pixels =
                SDL_MapRGBA(one->format, colour.r, colour.g, colour.b,
                            colour.a);
            SDL_BlitScaled(one, NULL, s, &dst);
        }
    }
}

/*--------------------------------------------------------------------------
 * The window half, which is `renwindow.c`'s and is only forwarded here
 *------------------------------------------------------------------------*/

void ren_init(SDL_Window *win)
{
    window_renderer.window = win;
    renwin_init_surface(&window_renderer);
    renwin_init_command_buf(&window_renderer);
    renwin_clip_to_surface(&window_renderer);
}

void ren_resize_window(RenWindow *window)
{
    renwin_resize_surface(window);
    renwin_update_scale(window);
}

void ren_update_rects(RenWindow *window, RenRect *rects, int count)
{
    renwin_update_rects(window, rects, count);
}

void ren_set_clip_rect(RenWindow *window, RenRect rect)
{
    renwin_set_clip_rect(window, rect);
}

void ren_get_size(RenWindow *window, int *x, int *y)
{
    RenSurface rs = renwin_get_surface(window);

    *x = (rs.surface != NULL) ? rs.surface->w / rs.scale : 0;
    *y = (rs.surface != NULL) ? rs.surface->h / rs.scale : 0;
}

void ren_free_window_resources(RenWindow *window)
{
    renwin_free(window);
}
