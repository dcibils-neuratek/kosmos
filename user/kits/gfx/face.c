/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Faces as a page is set with them (`docs/write.md`, W2).
 *
 *   gfx.typefaces()      every TrueType face the image carries:
 *                        { file, family, weight, italic } each
 *   gfx.typeface(file)   one of them, to measure with:
 *     face:metrics()     units to the em, ascent, descent, gap - in the
 *                        font's units, descent below the baseline negative
 *     face:advance(text) the advance of a UTF-8 string in the font's
 *                        units, and how many of its characters the face
 *                        has no glyph for
 *     face:program()     where the font's bytes are and how many: for the
 *                        PDF writer to embed (W3)
 *     face:descriptor()  what a PDF says about a font beside its widths:
 *                        its PostScript name, box, italic angle, cap
 *                        height, underline and strike-out, fixed pitch
 *     face:glyphs(text[, used])
 *                        a UTF-8 string as its glyph numbers, two bytes
 *                        each in hex - what a PDF's Identity-H shows - and
 *                        how many the face has no glyph for; `used[glyph]`
 *                        given each glyph's character, for its ToUnicode
 *     face:glyph_advance(glyph)
 *                        one glyph's advance, for a PDF's widths
 *     face:subset(glyphs, dst, cap)
 *                        the font with only those glyphs' outlines, into
 *                        a region: how many bytes (W3b)
 *
 * **In the font's own units, unhinted, and unscaled.** `gfx`'s outline
 * fonts measure at a pixel size, rounded per glyph, because they draw on a
 * screen. A page is set once for the screen and the PDF alike, and a PDF's
 * widths are the font's own advance widths - so this measures with exactly
 * those, and a line that fits in the PDF fits on the screen because both
 * were set by the same sums. The caller scales by the size in points.
 *
 * **What a face is called is what the font says**: its typographic family
 * (name 16, else name 1), its weight class from `OS/2`, and its italic bit
 * from `OS/2`'s selection. The Font menu lists what the fonts say they are,
 * and a font dropped into `assets/fonts/` is a family there with no table
 * here to learn about it.
 *
 * Only TrueType outlines - a font with a `glyf` table - because that is what
 * a PDF embeds as `FontFile2`. The image's fonts are all TrueType; one that
 * is not would be left out of the list rather than offered and then refused
 * at export.
 *
 * The bytes are the image's own, read-only and mapped into every process
 * from one copy, so a face points into them and copies nothing.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "stb_truetype.h"

struct kosmos_font_asset {
    const char          *name;
    const unsigned char *bytes;
    size_t               length;
};

extern const struct kosmos_font_asset fonts_table[];

#define FACE_MT "kosmos.face"

struct face {
    const struct kosmos_font_asset *asset;
    stbtt_fontinfo                  info;
    int                             units;      /* to the em */
};

static uint16_t be16(const unsigned char *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
           | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/*
 * A table of the font: where it starts and how long it is, inside the
 * font's bytes, or false. The image's fonts are trusted, and each offset
 * is still held to the length - the check costs nothing and a font is the
 * one kind of asset somebody adds by dropping a file in a folder.
 */
static bool table_of(const struct kosmos_font_asset *a, const char *tag,
                     uint32_t *at, uint32_t *len)
{
    unsigned n, i;

    if (a->length < 12) {
        return false;
    }

    n = be16(a->bytes + 4);

    if (12u + 16u * n > a->length) {
        return false;
    }

    for (i = 0; i < n; i++) {
        const unsigned char *r = a->bytes + 12 + 16 * i;

        if (memcmp(r, tag, 4) == 0) {
            uint32_t o = be32(r + 8), l = be32(r + 12);

            if (o > a->length || l > a->length - o) {
                return false;
            }

            *at = o;
            *len = l;
            return true;
        }
    }

    return false;
}

/*
 * A name from the `name` table as UTF-8: Windows' UTF-16 big-endian first,
 * which is what every modern font carries, and the Macintosh's single
 * bytes if that is all there is. Characters past the Basic Multilingual
 * Plane are not in any family name this system ships, and are dropped.
 */
static bool name_of(const stbtt_fontinfo *info, int id, char *out, size_t max)
{
    int len, i;
    size_t n = 0;
    const char *s = stbtt_GetFontNameString(info, &len, 3, 1, 0x409, id);

    if (s != NULL) {
        for (i = 0; i + 1 < len; i += 2) {
            unsigned c = ((unsigned)(unsigned char)s[i] << 8)
                         | (unsigned char)s[i + 1];

            if (c >= 0xd800 && c <= 0xdfff) {
                continue;
            }

            if (c < 0x80 && n + 1 < max) {
                out[n++] = (char)c;
            } else if (c < 0x800 && n + 2 < max) {
                out[n++] = (char)(0xc0 | (c >> 6));
                out[n++] = (char)(0x80 | (c & 0x3f));
            } else if (c >= 0x800 && n + 3 < max) {
                out[n++] = (char)(0xe0 | (c >> 12));
                out[n++] = (char)(0x80 | ((c >> 6) & 0x3f));
                out[n++] = (char)(0x80 | (c & 0x3f));
            }
        }
    } else {
        s = stbtt_GetFontNameString(info, &len, 1, 0, 0, id);

        if (s == NULL) {
            return false;
        }

        for (i = 0; i < len && n + 1 < max; i++) {
            if ((unsigned char)s[i] < 0x80) {
                out[n++] = s[i];
            }
        }
    }

    out[n] = '\0';
    return n > 0;
}

/* A font of the image as stb_truetype reads it, if it is a TrueType one. */
static bool open_asset(const struct kosmos_font_asset *a, stbtt_fontinfo *info)
{
    uint32_t at, len;
    int offset;

    if (!table_of(a, "glyf", &at, &len)) {
        return false;
    }

    offset = stbtt_GetFontOffsetForIndex(a->bytes, 0);

    return offset >= 0 && stbtt_InitFont(info, a->bytes, offset);
}

/* `gfx.typefaces()`: every TrueType face of the image, as the fonts name them. */
static int l_faces(lua_State *L)
{
    unsigned i;
    int n = 0;

    lua_newtable(L);

    for (i = 0; fonts_table[i].name != NULL; i++) {
        const struct kosmos_font_asset *a = &fonts_table[i];
        stbtt_fontinfo info;
        char family[96];
        uint32_t at, len;
        unsigned weight = 400;
        bool italic = false;

        if (!open_asset(a, &info)) {
            continue;
        }

        if (!name_of(&info, 16, family, sizeof(family))
            && !name_of(&info, 1, family, sizeof(family))) {
            continue;
        }

        /* usWeightClass at 4, fsSelection at 62 - bit 0 is ITALIC. */
        if (table_of(a, "OS/2", &at, &len) && len >= 64) {
            weight = be16(a->bytes + at + 4);
            italic = (be16(a->bytes + at + 62) & 1u) != 0;
        }

        lua_createtable(L, 0, 4);
        lua_pushstring(L, a->name);
        lua_setfield(L, -2, "file");
        lua_pushstring(L, family);
        lua_setfield(L, -2, "family");
        lua_pushinteger(L, (lua_Integer)weight);
        lua_setfield(L, -2, "weight");
        lua_pushboolean(L, italic);
        lua_setfield(L, -2, "italic");
        lua_rawseti(L, -2, ++n);
    }

    return 1;
}

/* `gfx.typeface(file)`: one face, by its file's name exactly. */
static int l_face(lua_State *L)
{
    const char *want = luaL_checkstring(L, 1);
    unsigned i;

    for (i = 0; fonts_table[i].name != NULL; i++) {
        const struct kosmos_font_asset *a = &fonts_table[i];
        struct face *f;
        uint32_t at, len;

        if (strcmp(a->name, want) != 0) {
            continue;
        }

        if (!table_of(a, "head", &at, &len) || len < 54) {
            return luaL_error(L, "gfx.typeface: %s has no head table", want);
        }

        f = lua_newuserdatauv(L, sizeof(*f), 0);
        memset(f, 0, sizeof(*f));
        f->asset = a;

        if (!open_asset(a, &f->info)) {
            return luaL_error(L, "gfx.typeface: %s is not a TrueType font", want);
        }

        f->units = be16(a->bytes + at + 18);

        if (f->units < 16 || f->units > 16384) {
            return luaL_error(L, "gfx.typeface: %s has %d units to the em", want,
                              f->units);
        }

        luaL_setmetatable(L, FACE_MT);
        return 1;
    }

    lua_pushnil(L);
    lua_pushfstring(L, "no face called %s in this image", want);
    return 2;
}

/* `face:metrics()`: units to the em, ascent, descent, gap (`hhea`'s). */
static int l_metrics(lua_State *L)
{
    struct face *f = luaL_checkudata(L, 1, FACE_MT);
    int ascent, descent, gap;

    stbtt_GetFontVMetrics(&f->info, &ascent, &descent, &gap);

    lua_pushinteger(L, f->units);
    lua_pushinteger(L, ascent);
    lua_pushinteger(L, descent);
    lua_pushinteger(L, gap);
    return 4;
}

/*
 * The next character of a UTF-8 string, and where the one after it starts.
 * A byte that does not begin a well-formed character is U+FFFD, one byte
 * long, so a broken string is measured rather than refused.
 */
static unsigned next_char(const unsigned char *s, size_t len, size_t *i)
{
    unsigned c = s[*i], need, cp;

    if (c < 0x80) {
        *i += 1;
        return c;
    }

    if (c >= 0xc2 && c <= 0xdf) {
        need = 1;
        cp = c & 0x1f;
    } else if (c >= 0xe0 && c <= 0xef) {
        need = 2;
        cp = c & 0x0f;
    } else if (c >= 0xf0 && c <= 0xf4) {
        need = 3;
        cp = c & 0x07;
    } else {
        *i += 1;
        return 0xfffd;
    }

    /* The character's other bytes are at i + 1 to i + need. */
    if (*i + need >= len) {
        *i += 1;
        return 0xfffd;
    }

    {
        unsigned k;

        for (k = 1; k <= need; k++) {
            unsigned b = s[*i + k];

            if ((b & 0xc0) != 0x80) {
                *i += 1;
                return 0xfffd;
            }

            cp = (cp << 6) | (b & 0x3f);
        }
    }

    *i += need + 1;
    return cp;
}

/*
 * `face:advance(text)`: the sum of the characters' advance widths in font
 * units, and how many had no glyph - drawn as the face's missing glyph,
 * whose advance is what is counted for them.
 */
static int l_advance(lua_State *L)
{
    struct face *f = luaL_checkudata(L, 1, FACE_MT);
    size_t len, i = 0;
    const unsigned char *s = (const unsigned char *)luaL_checklstring(L, 2, &len);
    lua_Integer total = 0, missing = 0;

    while (i < len) {
        unsigned cp = next_char(s, len, &i);
        int glyph = stbtt_FindGlyphIndex(&f->info, (int)cp);
        int advance, lsb;

        if (glyph == 0) {
            missing++;
        }

        stbtt_GetGlyphHMetrics(&f->info, glyph, &advance, &lsb);
        total += advance;
    }

    lua_pushinteger(L, total);
    lua_pushinteger(L, missing);
    return 2;
}

static int16_t s16(const unsigned char *p)
{
    return (int16_t)be16(p);
}

/*
 * `face:descriptor()`: what a PDF's `FontDescriptor` says, from the tables
 * that hold it - `head`'s box, `post`'s italic angle, underline and fixed
 * pitch, `OS/2`'s cap height and strike-out, and the PostScript name, which
 * is the font's `BaseFont`. In the font's units; a field whose table is
 * missing or too old to have it is left out, and the writer supplies what
 * the specification allows.
 */
static int l_descriptor(lua_State *L)
{
    struct face *f = luaL_checkudata(L, 1, FACE_MT);
    const unsigned char *b = f->asset->bytes;
    uint32_t at, len;
    char name[64];

    lua_createtable(L, 0, 12);

    if (name_of(&f->info, 6, name, sizeof(name))) {
        lua_pushstring(L, name);
        lua_setfield(L, -2, "postscript");
    }

    if (table_of(f->asset, "head", &at, &len) && len >= 54) {
        lua_pushinteger(L, s16(b + at + 36)); lua_setfield(L, -2, "xmin");
        lua_pushinteger(L, s16(b + at + 38)); lua_setfield(L, -2, "ymin");
        lua_pushinteger(L, s16(b + at + 40)); lua_setfield(L, -2, "xmax");
        lua_pushinteger(L, s16(b + at + 42)); lua_setfield(L, -2, "ymax");
    }

    /* italicAngle is a 16.16 fixed-point number of degrees. */
    if (table_of(f->asset, "post", &at, &len) && len >= 16) {
        int32_t angle = (int32_t)be32(b + at + 4);

        lua_pushnumber(L, (lua_Number)angle / 65536.0);
        lua_setfield(L, -2, "italic_angle");
        lua_pushinteger(L, s16(b + at + 8));
        lua_setfield(L, -2, "underline_position");
        lua_pushinteger(L, s16(b + at + 10));
        lua_setfield(L, -2, "underline_thickness");
        lua_pushboolean(L, be32(b + at + 12) != 0);
        lua_setfield(L, -2, "fixed");
    }

    if (table_of(f->asset, "OS/2", &at, &len) && len >= 30) {
        lua_pushinteger(L, s16(b + at + 26));
        lua_setfield(L, -2, "strike_size");
        lua_pushinteger(L, s16(b + at + 28));
        lua_setfield(L, -2, "strike_position");

        /* sCapHeight arrived with version 2 of the table. */
        if (be16(b + at) >= 2 && len >= 90) {
            lua_pushinteger(L, s16(b + at + 88));
            lua_setfield(L, -2, "cap_height");
        }
    }

    return 1;
}

/*
 * `face:glyphs(text[, used])`: each character's glyph as four hex digits,
 * which is how a PDF's Identity-H string says it, and how many characters
 * the face has no glyph for (glyph 0, drawn as the face's missing glyph).
 * When `used` is given, `used[glyph] = character` for each glyph the text
 * shows - the first character to reach it - which is the PDF's ToUnicode,
 * the map that lets its text be searched and copied. Glyph 0 is never
 * mapped: it stands for every character the face lacks, and so for none.
 *
 * In C because it is a loop over a document's characters, and it hands
 * back the hex rather than the bytes so no loop over them is left in Lua.
 */
static int l_glyphs(lua_State *L)
{
    static const char HEX[] = "0123456789ABCDEF";
    struct face *f = luaL_checkudata(L, 1, FACE_MT);
    size_t len, i = 0;
    const unsigned char *s = (const unsigned char *)luaL_checklstring(L, 2, &len);
    bool mapping = !lua_isnoneornil(L, 3);
    lua_Integer missing = 0;
    luaL_Buffer out;

    if (mapping) {
        luaL_checktype(L, 3, LUA_TTABLE);
    }

    luaL_buffinit(L, &out);

    while (i < len) {
        unsigned cp = next_char(s, len, &i);
        int glyph = stbtt_FindGlyphIndex(&f->info, (int)cp);
        char four[4];

        if (glyph == 0) {
            missing++;
        } else if (mapping && lua_rawgeti(L, 3, glyph) == LUA_TNIL) {
            lua_pop(L, 1);
            lua_pushinteger(L, (lua_Integer)cp);
            lua_rawseti(L, 3, glyph);
        } else if (mapping) {
            lua_pop(L, 1);
        }

        four[0] = HEX[(glyph >> 12) & 15];
        four[1] = HEX[(glyph >> 8) & 15];
        four[2] = HEX[(glyph >> 4) & 15];
        four[3] = HEX[glyph & 15];
        luaL_addlstring(&out, four, 4);
    }

    luaL_pushresult(&out);
    lua_pushinteger(L, missing);
    return 2;
}

/* `face:glyph_advance(glyph)`: one glyph's advance, in the font's units. */
static int l_glyph_advance(lua_State *L)
{
    struct face *f = luaL_checkudata(L, 1, FACE_MT);
    lua_Integer glyph = luaL_checkinteger(L, 2);
    int advance, lsb;

    if (glyph < 0 || glyph >= f->info.numGlyphs) {
        return luaL_error(L, "glyph %d is not in the face", (int)glyph);
    }

    stbtt_GetGlyphHMetrics(&f->info, (int)glyph, &advance, &lsb);
    lua_pushinteger(L, advance);
    return 1;
}

/*
 * **A subset of the font: only the outlines a document shows** (W3b).
 *
 * A face embedded whole is about 93 KB deflated, and a three-page document
 * in seven faces was 652 KB of font in a 664 KB PDF. Most of a font is its
 * outlines (`glyf`) and its positioning (`GPOS`), and a PDF shows a few
 * dozen of a thousand glyphs and positions them itself.
 *
 * **The glyph numbers do not change**: a PDF in Identity-H names glyphs by
 * the font's own numbers, so the subset keeps every number and empties the
 * outlines of the ones not shown - `loca` points them at nothing. Glyph 0
 * is kept, it being what a character the face lacks is drawn as, and so is
 * every glyph a kept composite is built from.
 *
 * Kept: `head` (its `loca` made long, its checksum made again), `hhea`,
 * `maxp`, `OS/2`, `hmtx`, `cmap`, `cvt `, `fpgm`, `prep` and `gasp` - what a
 * renderer, hinting or not, may read - and `post` cut to its header, version
 * 3, without names. Dropped: `GPOS`, `GSUB`, `GDEF`, `name`, `DSIG`, `meta`,
 * and anything else; a PDF reader uses none of them.
 */

#define SUBSET_MOST_TABLES 16

/* Composite glyph flags (the `glyf` table, OpenType). */
#define ARG_1_AND_2_ARE_WORDS     0x0001
#define WE_HAVE_A_SCALE           0x0008
#define MORE_COMPONENTS           0x0020
#define WE_HAVE_AN_X_AND_Y_SCALE  0x0040
#define WE_HAVE_A_TWO_BY_TWO      0x0080

struct glyphs_of {
    const unsigned char *glyf, *loca;
    uint32_t glyf_len, loca_len;
    unsigned count;
    bool long_loca;
};

/* Where glyph `g`'s outline is in `glyf`, and how long; false if `loca`
 * says something outside the table. */
static bool outline_of(const struct glyphs_of *t, unsigned g, uint32_t *at,
                       uint32_t *len)
{
    uint32_t a, b;

    if (t->long_loca) {
        if (4u * (g + 2u) > t->loca_len) return false;
        a = be32(t->loca + 4 * g);
        b = be32(t->loca + 4 * g + 4);
    } else {
        if (2u * (g + 2u) > t->loca_len) return false;
        a = 2u * be16(t->loca + 2 * g);
        b = 2u * be16(t->loca + 2 * g + 2);
    }

    if (b < a || b > t->glyf_len) return false;

    *at = a;
    *len = b - a;
    return true;
}

/* Glyph `g` kept, and every glyph it is built from, to a depth that no
 * real font reaches and a hostile one cannot pass. */
static void keep_glyph(const struct glyphs_of *t, unsigned char *keep,
                       unsigned g, unsigned depth)
{
    uint32_t at, len, p;
    uint16_t flags;

    if (g >= t->count || depth > 8) return;

    keep[g] = 1;

    if (!outline_of(t, g, &at, &len) || len < 10) return;

    /* A negative number of contours is a composite. */
    if (s16(t->glyf + at) >= 0) return;

    p = at + 10;

    do {
        if (p + 4 > at + len) return;

        flags = be16(t->glyf + p);
        keep_glyph(t, keep, be16(t->glyf + p + 2), depth + 1);

        p += 4 + ((flags & ARG_1_AND_2_ARE_WORDS) ? 4 : 2);

        if (flags & WE_HAVE_A_SCALE) p += 2;
        else if (flags & WE_HAVE_AN_X_AND_Y_SCALE) p += 4;
        else if (flags & WE_HAVE_A_TWO_BY_TWO) p += 8;
    } while (flags & MORE_COMPONENTS);
}

static uint32_t table_sum(const unsigned char *p, uint32_t len)
{
    uint32_t sum = 0, i;

    for (i = 0; i < len; i += 4) {
        unsigned char w[4] = { 0, 0, 0, 0 };
        uint32_t k;

        for (k = 0; k < 4 && i + k < len; k++) w[k] = p[i + k];

        sum += be32(w);
    }

    return sum;
}

static void put16(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

/*
 * `face:subset(glyphs, dst, cap)`: `glyphs` a list of glyph numbers, `dst`
 * and `cap` a mapped region; the subset's length, or an error when the
 * region is too small or the font's tables disagree with themselves.
 */
static int l_subset(lua_State *L)
{
    static const char *const KEEP[] = {
        "OS/2", "cmap", "cvt ", "fpgm", "gasp", "glyf", "head", "hhea",
        "hmtx", "loca", "maxp", "post", "prep",
    };
    struct face *f = luaL_checkudata(L, 1, FACE_MT);
    unsigned char *dst = (unsigned char *)(uintptr_t)luaL_checkinteger(L, 3);
    size_t cap = (size_t)luaL_checkinteger(L, 4);
    const struct kosmos_font_asset *a = f->asset;
    struct glyphs_of t;
    uint32_t at, len, head_at = 0;
    unsigned char *keep;
    unsigned n, i, tables = 0;
    size_t out, dir;
    lua_Integer k, many;

    luaL_checktype(L, 2, LUA_TTABLE);

    if (dst == NULL) {
        return luaL_error(L, "face:subset: needs a mapped region");
    }

    if (!table_of(a, "glyf", &at, &len)) {
        return luaL_error(L, "face:subset: no glyf");
    }

    t.glyf = a->bytes + at;
    t.glyf_len = len;

    if (!table_of(a, "loca", &at, &len)) {
        return luaL_error(L, "face:subset: no loca");
    }

    t.loca = a->bytes + at;
    t.loca_len = len;

    if (!table_of(a, "head", &at, &len) || len < 54) {
        return luaL_error(L, "face:subset: no head");
    }

    t.long_loca = s16(a->bytes + at + 50) != 0;

    if (!table_of(a, "maxp", &at, &len) || len < 6) {
        return luaL_error(L, "face:subset: no maxp");
    }

    t.count = be16(a->bytes + at + 4);

    /* Which glyphs keep their outlines. */
    keep = lua_newuserdatauv(L, t.count + 1u, 0);
    memset(keep, 0, t.count + 1u);
    keep_glyph(&t, keep, 0, 0);

    many = (lua_Integer)lua_rawlen(L, 2);

    for (k = 1; k <= many; k++) {
        lua_Integer g = (lua_rawgeti(L, 2, k), lua_tointeger(L, -1));

        lua_pop(L, 1);

        if (g > 0 && g < (lua_Integer)t.count) {
            keep_glyph(&t, keep, (unsigned)g, 0);
        }
    }

    /* The directory, then each table on a four-byte boundary. */
    for (i = 0; i < sizeof(KEEP) / sizeof(KEEP[0]); i++) {
        if (table_of(a, KEEP[i], &at, &len)) tables++;
    }

    dir = 12 + 16 * (size_t)tables;
    out = dir;

    if (cap < dir) {
        return luaL_error(L, "face:subset: no room");
    }

    memset(dst, 0, dir);
    put32(dst, 0x00010000u);
    put16(dst + 4, tables);

    {
        unsigned power = 1, log2 = 0;

        while (power * 2 <= tables) { power *= 2; log2++; }

        put16(dst + 6, power * 16);
        put16(dst + 8, log2);
        put16(dst + 10, tables * 16 - power * 16);
    }

    n = 0;

    for (i = 0; i < sizeof(KEEP) / sizeof(KEEP[0]); i++) {
        const char *tag = KEEP[i];
        size_t start = out;
        unsigned char *record;

        if (!table_of(a, tag, &at, &len)) continue;

        if (strcmp(tag, "glyf") == 0) {
            unsigned g;

            for (g = 0; g < t.count; g++) {
                uint32_t gat, glen;

                if (!keep[g] || !outline_of(&t, g, &gat, &glen)) continue;

                if (out + glen + 3 > cap) {
                    return luaL_error(L, "face:subset: no room");
                }

                memcpy(dst + out, t.glyf + gat, glen);
                out += glen;

                while ((out - start) & 3) dst[out++] = 0;
            }
        } else if (strcmp(tag, "loca") == 0) {
            unsigned g;
            uint32_t pos = 0;

            if (out + 4u * (t.count + 1u) > cap) {
                return luaL_error(L, "face:subset: no room");
            }

            for (g = 0; g <= t.count; g++) {
                uint32_t gat, glen;

                put32(dst + out + 4 * g, pos);

                if (g < t.count && keep[g] && outline_of(&t, g, &gat, &glen)) {
                    pos += (glen + 3u) & ~3u;
                }
            }

            out += 4u * (t.count + 1u);
        } else if (strcmp(tag, "post") == 0) {
            /* Version 3: the header, and no glyph names. */
            if (len < 32 || out + 32 > cap) {
                return luaL_error(L, "face:subset: post");
            }

            memcpy(dst + out, a->bytes + at, 32);
            put32(dst + out, 0x00030000u);
            out += 32;
        } else {
            if (out + len + 3 > cap) {
                return luaL_error(L, "face:subset: no room");
            }

            memcpy(dst + out, a->bytes + at, len);

            if (strcmp(tag, "head") == 0) {
                head_at = (uint32_t)out;
                put32(dst + out + 8, 0);            /* checkSumAdjustment */
                put16(dst + out + 50, 1);           /* indexToLocFormat: long */
            }

            out += len;
        }

        record = dst + 12 + 16 * n++;
        memcpy(record, tag, 4);
        put32(record + 4, table_sum(dst + start, (uint32_t)(out - start)));
        put32(record + 8, (uint32_t)start);
        put32(record + 12, (uint32_t)(out - start));

        while (out & 3) dst[out++] = 0;
    }

    /* The whole font's checksum, as `head` asks: 0xB1B0AFBA less the sum. */
    if (head_at != 0) {
        put32(dst + head_at + 8, 0xB1B0AFBAu - table_sum(dst, (uint32_t)out));
    }

    lua_pushinteger(L, (lua_Integer)out);
    return 1;
}

/* `face:program()`: the address of the font's bytes, and their length. */
static int l_program(lua_State *L)
{
    struct face *f = luaL_checkudata(L, 1, FACE_MT);

    lua_pushinteger(L, (lua_Integer)(uintptr_t)f->asset->bytes);
    lua_pushinteger(L, (lua_Integer)f->asset->length);
    return 2;
}

void kosmos_face_open(lua_State *L)
{
    /* Into the `gfx` table, on the stack: the fonts are `gfx`'s, embedded
     * by the build as `fonts_table`. */
    luaL_newmetatable(L, FACE_MT);

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, l_metrics); lua_setfield(L, -2, "metrics");
    lua_pushcfunction(L, l_advance); lua_setfield(L, -2, "advance");
    lua_pushcfunction(L, l_program); lua_setfield(L, -2, "program");
    lua_pushcfunction(L, l_descriptor); lua_setfield(L, -2, "descriptor");
    lua_pushcfunction(L, l_glyphs);  lua_setfield(L, -2, "glyphs");
    lua_pushcfunction(L, l_glyph_advance);
    lua_setfield(L, -2, "glyph_advance");
    lua_pushcfunction(L, l_subset);  lua_setfield(L, -2, "subset");

    lua_pop(L, 1);

    /*
     * **Two names `gfx` did not have.** These were `faces` and `face` the
     * first time, and `gfx.face` was taken - the screen's faces by short
     * name and pixel size, which `ui.lua`, the window manager and the
     * browser all draw with - so this quietly replaced it and every window
     * lost its text (`testing.md` 18.378). Refused here now, at open,
     * rather than found by a desktop with no words on it.
     */
    {
        static const char *const names[] = { "typefaces", "typeface" };
        static const lua_CFunction calls[] = { l_faces, l_face };
        unsigned i;

        for (i = 0; i < 2; i++) {
            if (lua_getfield(L, -1, names[i]) != LUA_TNIL) {
                luaL_error(L, "gfx.%s is taken already", names[i]);
            }

            lua_pop(L, 1);
            lua_pushcfunction(L, calls[i]);
            lua_setfield(L, -2, names[i]);
        }
    }
}
