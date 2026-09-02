/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Fonts that arrive inside a document, and drawing a page of them at once.
 *
 * `gfx`'s own outline fonts are the three the system ships, cached by
 * codepoint from 32 to 126, loaded from the image. A PDF's fonts are none of
 * those things: they come as bytes inside the file, they are addressed by
 * *glyph index* rather than by character, and a page uses about a hundred of
 * the several thousand a face contains.
 *
 * **Written for scrolling, not for correctness alone.** A page of the
 * document this was built against is 663 shows and roughly two thousand
 * glyphs. Three numbers decide whether turning a page feels instant:
 *
 *   1. **A glyph is rasterised once.** Times New Roman at one size is about
 *      a hundred distinct glyphs on a page, so a cache turns two thousand
 *      outline rasterisations into a hundred, and the rest into blits of a
 *      coverage bitmap - which is what the blitter is already good at.
 *
 *   2. **A page is one crossing, not two thousand.** `gfx.md` 19.11 puts a
 *      Lua/C crossing at about two thousand pixels of work, so a call per
 *      glyph would cost more than the drawing. `draw` takes the whole page
 *      as a flat array of glyph, x, y and walks it here.
 *
 *   3. **The page is drawn once and scrolled as pixels.** That part is not
 *      in this file - it is the caller's - but it is the reason this one
 *      does not need to be fast twice. Scrolling blits a rectangle out of a
 *      surface that already holds the rendered page; nothing here runs
 *      again until the page changes.
 *
 * The bytes are not copied and are not a Lua string. `stb_truetype` reads
 * from the buffer for the life of the font, so the caller inflates the
 * font program into a region and passes the address - the same no-copy path
 * the content stream takes. Freeing that region while a font still points
 * into it is a use-after-free, which is why `free` exists and why the
 * comment on it says so.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "kosmos.h"

#include "stb_truetype.h"

/*
 * A note that `stb_impl.c` anticipated and this file makes live.
 *
 * Its comment says the fonts this system loads are vendored in its own
 * image, so stb_truetype's "DO NOT USE THIS ON UNTRUSTED FONT FILES" is not
 * a risk today - and adds that a font from a disk should still be treated
 * as hostile. This is that day: a PDF carries its own font programs and a
 * PDF is a file somebody else made.
 *
 * The mitigation is the architecture rather than a promise about the
 * parser. This runs at EL0, inside whichever process asked to draw, with
 * its own address space and its own capabilities - so a font crafted to
 * exploit the rasteriser kills the viewer and reaches nothing else. That is
 * the microkernel doing the job it exists for, and it is worth saying out
 * loud rather than relying on quietly.
 */

#define DOCFONT_MT  "kosmos.docfont"

/*
 * How many distinct glyphs are kept.
 *
 * A page of prose uses on the order of a hundred: the alphabet in two
 * cases, digits, punctuation, and whatever ligatures the typesetter
 * reached for. 256 is comfortably past that and is one page of memory for
 * the table itself.
 *
 * Open addressing with linear probing, and on a full table the oldest entry
 * for that slot is simply replaced. A cache that refuses to evict would
 * stop working on a document with two alphabets in it; one that evicts
 * badly just rasterises a glyph again.
 */
#define CACHE_SLOTS  256

struct cached {
    int            glyph;        /* -1 for an empty slot */
    unsigned char *coverage;     /* w * h bytes, or NULL for a blank */
    size_t         pages;        /* what to hand back */
    int            w, h;
    int            xoff, yoff;   /* from the pen, at the baseline */
};

struct docfont {
    stbtt_fontinfo info;
    const unsigned char *bytes;
    float          scale;
    int            px;
    int            ascent, descent, line_gap;
    struct cached  cache[CACHE_SLOTS];
    unsigned       rasterised;   /* how many went through stb_truetype */
    unsigned       drawn;        /* how many were put on a surface */
};

/*
 * The surface, as `gfx.c` lays it out.
 *
 * Repeated here rather than shared through a header, exactly as
 * `struct message` is repeated between the kernel and userland: two files
 * that must agree, and a static assert would be better than a comment the
 * day this moves. It is checked by metatable name, so a mismatch is a
 * refusal rather than a wrong pointer.
 */
struct surface_ref {
    uint32_t *pixels;
    unsigned  width;
    unsigned  height;
    unsigned  pitch;
};

static struct docfont *check_font(lua_State *L, int index)
{
    return luaL_checkudata(L, index, DOCFONT_MT);
}

static void *map_bytes(size_t bytes, size_t *pages_out)
{
    size_t pages = (bytes + KOSMOS_PAGE_SIZE - 1) / KOSMOS_PAGE_SIZE;
    long   mapped;

    if (pages == 0) {
        pages = 1;
    }

    mapped = kosmos_map(pages);

    if (mapped < 0) {
        return NULL;
    }

    *pages_out = pages;
    return (void *)(uintptr_t)mapped;
}

static void cache_clear(struct docfont *f)
{
    unsigned i;

    for (i = 0; i < CACHE_SLOTS; i++) {
        if (f->cache[i].coverage != NULL) {
            kosmos_unmap((uintptr_t)f->cache[i].coverage, f->cache[i].pages);
        }

        f->cache[i].coverage = NULL;
        f->cache[i].pages = 0;
        f->cache[i].glyph = -1;
    }
}

/*
 * The glyph, rasterised if this is the first time it has been asked for.
 *
 * Returns NULL only when the outline could not be produced at all. A glyph
 * with no ink - a space - is a valid answer with `coverage` NULL and an
 * advance, and must not be confused with a failure.
 */
static struct cached *glyph_of(struct docfont *f, int glyph)
{
    unsigned slot = ((unsigned)glyph * 2654435761u) % CACHE_SLOTS;
    unsigned probe;
    struct cached *c;
    unsigned char *bitmap;
    int w = 0, h = 0, xoff = 0, yoff = 0;

    for (probe = 0; probe < 8; probe++) {
        c = &f->cache[(slot + probe) % CACHE_SLOTS];

        if (c->glyph == glyph) {
            return c;
        }

        if (c->glyph < 0) {
            break;
        }
    }

    /* Either an empty slot, or the eighth probe: replace whatever is there. */
    c = &f->cache[(slot + (probe < 8 ? probe : 0)) % CACHE_SLOTS];

    if (c->coverage != NULL) {
        kosmos_unmap((uintptr_t)c->coverage, c->pages);
        c->coverage = NULL;
        c->pages = 0;
    }

    bitmap = stbtt_GetGlyphBitmap(&f->info, f->scale, f->scale, glyph,
                                  &w, &h, &xoff, &yoff);

    c->glyph = glyph;
    c->w = w;
    c->h = h;
    c->xoff = xoff;
    c->yoff = yoff;
    c->coverage = NULL;
    c->pages = 0;

    f->rasterised++;

    if (bitmap == NULL || w <= 0 || h <= 0) {
        /* A space, or a glyph with no outline. Cached as blank so it is not
         * asked of stb_truetype again on every page. */
        if (bitmap != NULL) {
            stbtt_FreeBitmap(bitmap, NULL);
        }
        return c;
    }

    {
        size_t need = (size_t)w * (size_t)h;
        size_t pages = 0;
        unsigned char *own = map_bytes(need, &pages);

        if (own != NULL) {
            memcpy(own, bitmap, need);
            c->coverage = own;
            c->pages = pages;
        }
    }

    stbtt_FreeBitmap(bitmap, NULL);
    return c;
}

/* Coverage over a colour, the same blend `gfx.c` uses for its own glyphs. */
static inline uint32_t mix(uint32_t dst, uint32_t src, unsigned a)
{
    unsigned inv = 255u - a;

    unsigned r = (((src >> 16) & 0xff) * a + ((dst >> 16) & 0xff) * inv) / 255u;
    unsigned g = (((src >>  8) & 0xff) * a + ((dst >>  8) & 0xff) * inv) / 255u;
    unsigned b = (((src      ) & 0xff) * a + ((dst      ) & 0xff) * inv) / 255u;

    return 0xff000000u | (r << 16) | (g << 8) | b;
}

/*
 * `font:draw(surface, colour, runs)` - a whole page in one crossing.
 *
 * `runs` is a flat array of glyph, x, y, glyph, x, y... in surface pixels,
 * with y the *baseline* rather than the top, because that is what a text
 * renderer produces and converting it in Lua would be arithmetic per glyph
 * on the wrong side of the boundary.
 *
 * Flat on purpose. A table per glyph would be three thousand tables for a
 * page and the collector would walk every one of them; `gfx.md` makes the
 * same argument for pixels and it is the same argument here.
 */
static int l_draw(lua_State *L)
{
    struct docfont *f = check_font(L, 1);
    struct surface_ref *s = luaL_checkudata(L, 2, "kosmos.surface");
    uint32_t colour = (uint32_t)luaL_checkinteger(L, 3);
    lua_Integer n;
    lua_Integer i;
    unsigned drawn = 0;

    luaL_checktype(L, 4, LUA_TTABLE);

    if (s->pixels == NULL) {
        return luaL_error(L, "docfont:draw: that surface has been freed");
    }

    n = (lua_Integer)lua_rawlen(L, 4);

    for (i = 1; i + 2 <= n; i += 3) {
        struct cached *c;
        int glyph, ox, oy, gx, gy;

        lua_rawgeti(L, 4, i);
        glyph = (int)lua_tointeger(L, -1);
        lua_rawgeti(L, 4, i + 1);
        ox = (int)lua_tointeger(L, -1);
        lua_rawgeti(L, 4, i + 2);
        oy = (int)lua_tointeger(L, -1);
        lua_pop(L, 3);

        c = glyph_of(f, glyph);

        if (c == NULL || c->coverage == NULL) {
            continue;               /* a space, or no outline */
        }

        for (gy = 0; gy < c->h; gy++) {
            long py = (long)oy + c->yoff + gy;
            uint32_t *row;

            if (py < 0 || py >= (long)s->height) {
                continue;
            }

            row = (uint32_t *)((char *)s->pixels + (size_t)py * s->pitch);

            for (gx = 0; gx < c->w; gx++) {
                long px = (long)ox + c->xoff + gx;
                unsigned a = c->coverage[gy * c->w + gx];

                if (px < 0 || px >= (long)s->width || a == 0) {
                    continue;
                }

                row[px] = (a == 255) ? colour : mix(row[px], colour, a);
            }
        }

        drawn++;
    }

    f->drawn += drawn;

    lua_pushinteger(L, (lua_Integer)drawn);
    return 1;
}

/* `font:metrics()` - ascent, descent and line gap, in pixels at this size. */
static int l_metrics(lua_State *L)
{
    struct docfont *f = check_font(L, 1);

    lua_newtable(L);
    lua_pushinteger(L, f->ascent);   lua_setfield(L, -2, "ascent");
    lua_pushinteger(L, -f->descent); lua_setfield(L, -2, "descent");
    lua_pushinteger(L, f->line_gap); lua_setfield(L, -2, "line_gap");
    lua_pushinteger(L, f->px);       lua_setfield(L, -2, "px");
    return 1;
}

/*
 * `font:stats()` - how many glyphs were rasterised against how many drawn.
 *
 * The whole justification for the cache is the ratio between these two, and
 * a number nobody can read is a claim rather than a measurement.
 */
static int l_stats(lua_State *L)
{
    struct docfont *f = check_font(L, 1);

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)f->rasterised);
    lua_setfield(L, -2, "rasterised");
    lua_pushinteger(L, (lua_Integer)f->drawn);
    lua_setfield(L, -2, "drawn");
    return 1;
}

static int l_free(lua_State *L)
{
    struct docfont *f = check_font(L, 1);

    cache_clear(f);
    f->bytes = NULL;
    return 0;
}

static int l_gc(lua_State *L)
{
    struct docfont *f = luaL_testudata(L, 1, DOCFONT_MT);

    if (f != NULL) {
        cache_clear(f);
    }

    return 0;
}

/*
 * `gfx.docfont(address, length, capacity, px)` - a font out of a document.
 *
 * `capacity` is how much room the region has, because a subset font may
 * need four bytes appended; see `ensure_cmap`.
 *
 * The address is a mapped region holding the font program, and it must stay
 * mapped and unchanged for as long as the font is used: `stb_truetype`
 * reads from it on every glyph rather than taking a copy. That is why this
 * takes an address rather than a string - a 400 KB font program as a Lua
 * string is 400 KB of a 2 MB heap, for bytes nothing in Lua will ever look
 * at.
 */
static inline uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static inline void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

/*
 * A font out of a PDF has no `cmap`, and stb_truetype insists on one.
 *
 * That is not a broken font. A CID-keyed subset is addressed by *glyph
 * index* - the content stream says "draw glyph 3" - so a table mapping
 * characters to glyphs means nothing and the producer drops it. The font
 * this was found on has eleven tables and `cmap` is not among them.
 *
 * `stbtt_InitFont` fails outright without one - and, it turns out, fails
 * again if the cmap contains no encoding record it recognises: the last
 * thing it does is `if (info->index_map == 0) return 0`. So an empty table
 * is not enough. It needs one Microsoft/Unicode-BMP record pointing at a
 * subtable, and the subtable may be empty, because nothing here ever asks
 * for a codepoint - the content stream names glyphs by index.
 *
 * Twenty-two bytes: a four-byte header, an eight-byte encoding record, and
 * a ten-byte format 6 subtable that maps nothing.
 *
 * Rather than modify `stb_truetype.h`, which is vendored and stays as its
 * author released it, this edits *our copy of the input*: four zero bytes
 * appended - a cmap header declaring no encodings - and one directory entry
 * repointed at them. The entry taken is `post`, which carries glyph names
 * and PostScript metadata and is not read by the rasteriser; `OS/2` and
 * `name` are the fallbacks for the same reason.
 *
 * The font's checksums are wrong afterwards. Nothing checks them.
 */
static bool ensure_cmap(unsigned char *data, size_t len, size_t cap,
                        size_t *out_len)
{
    static const char *droppable[] = { "post", "OS/2", "name", "cvt ", NULL };

    unsigned tables;
    unsigned i, d;

    *out_len = len;

    if (len < 12) {
        return false;
    }

    tables = ((unsigned)data[4] << 8) | data[5];

    if (12u + 16u * tables > len) {
        return false;
    }

    for (i = 0; i < tables; i++) {
        if (memcmp(data + 12 + 16 * i, "cmap", 4) == 0) {
            return true;                    /* already has one */
        }
    }

    if (cap < len + 22) {
        return false;                       /* nowhere to put the table */
    }

    for (d = 0; droppable[d] != NULL; d++) {
        for (i = 0; i < tables; i++) {
            unsigned char *entry = data + 12 + 16 * i;

            if (memcmp(entry, droppable[d], 4) != 0) {
                continue;
            }

            {
                unsigned char *c = data + len;

                c[0] = 0; c[1] = 0;             /* version 0 */
                c[2] = 0; c[3] = 1;             /* one encoding record */

                c[4] = 0; c[5] = 3;             /* platform 3, Microsoft */
                c[6] = 0; c[7] = 1;             /* encoding 1, Unicode BMP */
                put32(c + 8, 12u);              /* subtable, from cmap start */

                c[12] = 0; c[13] = 6;           /* format 6 */
                c[14] = 0; c[15] = 10;          /* its length */
                c[16] = 0; c[17] = 0;           /* language */
                c[18] = 0; c[19] = 0;           /* first code */
                c[20] = 0; c[21] = 0;           /* entry count: none */
            }

            memcpy(entry, "cmap", 4);
            put32(entry + 8, (uint32_t)len);    /* offset */
            put32(entry + 12, 22u);             /* length */

            *out_len = len + 22;
            return true;
        }
    }

    return false;
}

static int l_docfont(lua_State *L)
{
    uintptr_t at  = (uintptr_t)luaL_checkinteger(L, 1);
    size_t    len = (size_t)luaL_checkinteger(L, 2);
    size_t    cap = (size_t)luaL_checkinteger(L, 3);
    int       px  = (int)luaL_checkinteger(L, 4);
    struct docfont *f;
    int offset;

    if (at == 0 || len == 0) {
        return luaL_error(L, "gfx.docfont: no font program");
    }

    if (cap < len) {
        cap = len;
    }

    if (!ensure_cmap((unsigned char *)at, len, cap, &len)) {
        return luaL_error(L,
            "gfx.docfont: no cmap and nowhere to synthesise one");
    }

    (void)be32;

    if (px < 4 || px > 256) {
        return luaL_error(L, "gfx.docfont: %d px is outside 4..256", px);
    }

    f = lua_newuserdatauv(L, sizeof(*f), 0);
    memset(f, 0, sizeof(*f));

    {
        unsigned i;
        for (i = 0; i < CACHE_SLOTS; i++) {
            f->cache[i].glyph = -1;
        }
    }

    f->bytes = (const unsigned char *)at;

    offset = stbtt_GetFontOffsetForIndex(f->bytes, 0);

    if (offset < 0 || !stbtt_InitFont(&f->info, f->bytes, offset)) {
        return luaL_error(L, "gfx.docfont: those bytes are not a font");
    }

    f->px = px;
    f->scale = stbtt_ScaleForPixelHeight(&f->info, (float)px);

    {
        int a, d, g;

        stbtt_GetFontVMetrics(&f->info, &a, &d, &g);

        f->ascent   = (int)((float)a * f->scale);
        f->descent  = (int)((float)d * f->scale);
        f->line_gap = (int)((float)g * f->scale);
    }

    luaL_getmetatable(L, DOCFONT_MT);
    lua_setmetatable(L, -2);

    return 1;
}

void kosmos_docfont_open(lua_State *L)
{
    /* Into the `gfx` table, which is on the stack: this draws onto a
     * surface and has to know what one is, which is what keeps it here
     * rather than in a kit of its own. */
    luaL_newmetatable(L, DOCFONT_MT);

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, l_draw);    lua_setfield(L, -2, "draw");
    lua_pushcfunction(L, l_metrics); lua_setfield(L, -2, "metrics");
    lua_pushcfunction(L, l_stats);   lua_setfield(L, -2, "stats");
    lua_pushcfunction(L, l_free);    lua_setfield(L, -2, "free");
    lua_pushcfunction(L, l_gc);      lua_setfield(L, -2, "__gc");

    lua_pop(L, 1);

    lua_pushcfunction(L, l_docfont);
    lua_setfield(L, -2, "docfont");
}
