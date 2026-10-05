/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Flate, for whoever asks: `compress.inflate(bytes) -> string`.
 *
 * `png.c` has inflated its own image data since M6, because a PNG's pixels
 * arrive compressed and a decoder that cannot decompress them is not a
 * decoder. A PDF wants exactly the same thing for a different reason - 292
 * of the 294 streams in the document this was written against are
 * FlateDecode - and the answer should not be a second copy of the call.
 *
 * So the deflate that was private to the image decoder becomes something
 * any process can ask for. The inflater is miniz's `tinfl`, vendored
 * unmodified and run in `gzip.c` - the same one a gzip stream goes through,
 * so the system has one inflater rather than two - and this file is what
 * makes it reachable from Lua, and the state it works in.
 *
 * **Why this is C and the PDF parser is not.** `design.md` 6 draws the line
 * at loops over bytes, and inflate is the definition of one: a bit reader,
 * a Huffman decode per symbol, and a copy per match. The parser above it
 * walks a structure and makes decisions, which is the other side of the
 * same line. The measurement that settled that argument for the journal -
 * structure in Lua costs 2%, a byte loop in Lua costs 30% - is the reason
 * this file exists rather than a Lua inflate.
 */

#include <stddef.h>
#include <stdint.h>

#include "lua.h"
#include "lauxlib.h"

#include "kosmos.h"
#include "gzip.h"

/*
 * The ceiling on one call.
 *
 * Not arbitrary: the result becomes a Lua string, and a process has a 2 MB
 * heap by design (`design.md` 5.2, to keep collections short). A stream
 * that inflates past this cannot be returned as a string whatever this file
 * does, so the limit is stated here where the error can say so, rather than
 * discovered as "not enough memory" three frames up with nothing naming the
 * cause.
 *
 * The way past it is `inflate_into`, below: into a region the caller
 * already owns, with no Lua string at either end.
 */
#define INFLATE_MAX (1024u * 1024u)

/*
 * The inflater's state and its 32 KB window (`gzip.h`), in pages of this
 * process's own - about 43 KB, mapped the first time it is wanted and kept,
 * as `deflate.c` keeps the deflater's. Not on the Lua heap: a PDF inflates
 * a stream for every page it draws and every font on it, and 43 KB of
 * garbage each time would be the collector's to walk for nothing.
 */
static struct gunzip_work *kept;

struct gunzip_work *kosmos_inflater(void)
{
    if (kept == NULL) {
        size_t pages = 0;

        kept = kosmos_map_bytes(sizeof(*kept), &pages);
    }

    return kept;
}

/*
 * A zlib stream is a two-byte header and then raw deflate, and the inflater
 * wants the deflate. The header is checked rather than assumed away: the low
 * nibble of the first byte is the compression method and must be 8, and the
 * two bytes together are a multiple of 31. What follows the deflate - the
 * Adler-32 - is not read, as it never was: a PDF whose producer got it wrong
 * still has its page.
 *
 * When it does not look like zlib the bytes are treated as raw deflate
 * instead of refused. That is not tidiness, it is a real case - producers
 * exist that write a bare deflate stream and call it FlateDecode - and
 * guessing wrong here costs nothing, because the inflater will refuse data
 * that is neither.
 */
static const unsigned char *skip_zlib_header(const unsigned char *p, size_t *len)
{
    if (*len >= 2 && (p[0] & 0x0f) == 8 && ((p[0] << 8) | p[1]) % 31 == 0) {
        *len -= 2;
        return p + 2;
    }

    return p;
}

static int l_inflate(lua_State *L)
{
    size_t               source_len = 0;
    const unsigned char *source     = (const unsigned char *)
                                      luaL_checklstring(L, 1, &source_len);

    struct gunzip_work *work = kosmos_inflater();
    size_t         destlen = 0;
    size_t         got = 0;
    unsigned char *dest;
    size_t         pages = 0;
    int            err;

    source = skip_zlib_header(source, &source_len);

    if (source_len == 0) {
        return luaL_error(L, "inflate: no compressed data");
    }

    if (work == NULL) {
        return luaL_error(L, "inflate: no room for the inflater");
    }

    /*
     * Twice, on purpose. The first pass decodes through the window and keeps
     * nothing, which is the only honest way to learn how much a stream
     * expands to: the alternative is guessing a multiple, growing on
     * failure, and inflating the beginning repeatedly. It means the pages
     * the string is made from are exact rather than nearly right, and that
     * nothing past `INFLATE_MAX` is ever mapped.
     */
    err = kosmos_inflated_size(source, source_len, work, &destlen);
    if (err != GUNZIP_WHOLE) {
        return luaL_error(L, "inflate: the data would not inflate (%d)", err);
    }

    if (destlen == 0) {
        lua_pushliteral(L, "");
        return 1;
    }

    if (destlen > INFLATE_MAX) {
        return luaL_error(L,
            "inflate: %d bytes is past the %d this can return as a string",
            (int)destlen, (int)INFLATE_MAX);
    }

    dest = kosmos_map_bytes(destlen, &pages);
    if (dest == NULL) {
        return luaL_error(L, "inflate: no room for %d bytes", (int)destlen);
    }

    err = kosmos_inflate(source, source_len, work, dest, destlen, &got);
    if (err != GUNZIP_WHOLE || got != destlen) {
        kosmos_unmap((uintptr_t)dest, pages);
        return luaL_error(L, "inflate: the second pass disagreed (%d)", err);
    }

    lua_pushlstring(L, (const char *)dest, (size_t)destlen);

    /* The string is a copy on the Lua heap, so the pages go back now. */
    kosmos_unmap((uintptr_t)dest, pages);

    return 1;
}

/*
 * `compress.inflate_into(src, src_bytes, dst, dst_bytes [, raw]) -> written`
 *
 * The same inflate with no Lua string at either end.
 *
 * The string form above copies three times over: the pages the filesystem
 * filled become a Lua string, the string is inflated into a second string,
 * and the parser walks that. All three are on a 2 MB heap, and the last two
 * exist only because the boundary is a string. Given the addresses of two
 * regions this process already owns, the compressed bytes are read where
 * the filesystem put them and the result is written where the parser will
 * read it, and the heap sees neither.
 *
 * That is the same argument `gfx.wrap` makes for pixels, applied to
 * document bytes: `CLAUDE.md` says pixels never go in a Lua table, and the
 * reason - a copy the size of the data, on a heap that cannot afford one -
 * is not specific to pixels.
 *
 * The addresses come from `sys.memory_map`, and Lua cannot do anything with
 * them except hand them back. A wrong one faults, and a fault at EL0 kills
 * this process and nothing else - which is the same exposure `gfx.wrap`
 * already carries and the reason it is tolerable.
 */
static int l_inflate_into(lua_State *L)
{
    uintptr_t      src     = (uintptr_t)luaL_checkinteger(L, 1);
    size_t         src_len = (size_t)luaL_checkinteger(L, 2);
    uintptr_t      dst     = (uintptr_t)luaL_checkinteger(L, 3);
    size_t         dst_cap = (size_t)luaL_checkinteger(L, 4);

    /*
     * **`raw`, the fifth: raw deflate, said rather than guessed.** A zip's
     * data is deflate with no zlib header, and the guess below - a header
     * when the first two bytes look like one - is a guess a raw stream can
     * lose. So a zip says what it holds (`roadmap.md` 6v).
     */
    int            raw     = lua_toboolean(L, 5);

    const unsigned char *source;
    struct gunzip_work  *work = kosmos_inflater();
    size_t               destlen = 0;
    int                  err;

    if (src == 0 || dst == 0) {
        return luaL_error(L, "inflate_into: needs two mapped regions");
    }

    if (src_len == 0) {
        return luaL_error(L, "inflate_into: no compressed data");
    }

    if (work == NULL) {
        return luaL_error(L, "inflate_into: no room for the inflater");
    }

    source = raw ? (const unsigned char *)src
                 : skip_zlib_header((const unsigned char *)src, &src_len);

    /*
     * Once, straight into the region: the stream's own output is its window.
     * It used to be decoded twice, the first time only to be sure it fitted;
     * now a stream that does not fit stops at the region's end, and is
     * decoded again only then, to say how much room it wanted. What it wrote
     * before it stopped is the caller's region and nobody else's.
     */
    err = kosmos_inflate(source, src_len, work, (unsigned char *)dst, dst_cap,
                         &destlen);

    if (err == GUNZIP_REFUSED) {
        err = kosmos_inflated_size(source, src_len, work, &destlen);

        if (err == GUNZIP_WHOLE) {
            return luaL_error(L,
                "inflate_into: %d bytes will not fit in %d",
                (int)destlen, (int)dst_cap);
        }
    }

    if (err != GUNZIP_WHOLE) {
        return luaL_error(L, "inflate_into: would not inflate (%d)", err);
    }

    lua_pushinteger(L, (lua_Integer)destlen);
    return 1;
}

/*
 * `compress.inflated_size(src, src_bytes) -> bytes`
 *
 * How big the result will be, without producing it. A caller needs this to
 * know whether the region it has is large enough, and asking costs a
 * decode into the inflater's own window, kept nowhere.
 */
static int l_inflated_size(lua_State *L)
{
    uintptr_t src     = (uintptr_t)luaL_checkinteger(L, 1);
    size_t    src_len = (size_t)luaL_checkinteger(L, 2);

    const unsigned char *source;
    struct gunzip_work  *work = kosmos_inflater();
    size_t               destlen = 0;
    int                  err;

    if (src == 0 || src_len == 0) {
        return luaL_error(L, "inflated_size: nothing to measure");
    }

    if (work == NULL) {
        return luaL_error(L, "inflated_size: no room for the inflater");
    }

    source = skip_zlib_header((const unsigned char *)src, &src_len);

    err = kosmos_inflated_size(source, src_len, work, &destlen);
    if (err != GUNZIP_WHOLE) {
        return luaL_error(L, "inflated_size: would not inflate (%d)", err);
    }

    lua_pushinteger(L, (lua_Integer)destlen);
    return 1;
}

/*
 * `compress.gunzip(bytes [, most]) -> text [, why]`
 *
 * A gzip stream inflated (`gzip.c`) - a web page, when the server was told
 * it may send one compressed. `text` alone when every member inflated and
 * checked out; `text` and why when it stopped - cut short, a check that
 * does not match, data that is not deflate - `text` being what it inflated
 * to before it did; nil and why for bytes that are not gzip at all.
 *
 * `most`, the most it may inflate to, is the caller's: a few kilobytes can
 * say they are gigabytes, and only the caller knows how much it can hold.
 * Without it, as much as the stream says.
 *
 * Into a Lua string, since that is what the parser is handed today; the
 * HTTP Kit that keeps a page's bytes out of Lua (`roadmap.md` 6zz g) will
 * put them somewhere else, and `gzip.c` does not care where.
 */
struct gunzip_into {
    luaL_Buffer *b;
    size_t       most, got;
};

static int put_into(void *user, const uint8_t *bytes, size_t n)
{
    struct gunzip_into *into = user;

    if (into->most != 0 && into->got + n > into->most) {
        return 0;
    }

    luaL_addlstring(into->b, (const char *)bytes, n);
    into->got += n;

    return 1;
}

static int l_gunzip(lua_State *L)
{
    size_t len = 0;
    const uint8_t *src = (const uint8_t *)luaL_checklstring(L, 1, &len);
    lua_Integer most = luaL_optinteger(L, 2, 0);

    /* The inflater's state and window, about 43 KB, on the Lua heap and
     * collected with the call - pushed before the buffer, which owns the
     * stack above it until it is done. */
    struct gunzip_work *work = lua_newuserdatauv(L, sizeof(*work), 0);
    luaL_Buffer b;
    struct gunzip_into into = { &b, most > 0 ? (size_t)most : 0, 0 };
    size_t out = 0;
    int result;

    luaL_buffinit(L, &b);
    result = kosmos_gunzip(src, len, work, put_into, &into, &out);
    luaL_pushresult(&b);

    if (result == GUNZIP_WHOLE) {
        return 1;
    }

    if (result == GUNZIP_NOT_GZIP) {
        lua_pushnil(L);
    }

    lua_pushstring(L, kosmos_gunzip_said(result));
    return 2;
}

/*
 * `compress.gunzip_stream([most]) -> g`, the same read as the bytes arrive
 * (`roadmap.md` 6zz l1), for a page handed to the parser while it comes.
 *
 *   g:feed(bytes) -> text, or nil and why once it has failed
 *   g:finish()    -> true, or false and why: what `gunzip` would have said
 *
 * `text` is what these bytes inflated to - often nothing, when a piece
 * ends inside a header or a block - and `most` the ceiling on all of it
 * together, as `gunzip`'s is. The state and the 32 KB window live in the
 * userdata, so nothing is allocated past it and nothing needs freeing.
 */
#define GUNZIP_STREAM "kosmos.gunzip_stream"

struct gunzip_lua {
    struct gunzip_stream s;
    size_t               most;
};

static int l_gunzip_stream(lua_State *L)
{
    lua_Integer most = luaL_optinteger(L, 1, 0);
    struct gunzip_lua *g = lua_newuserdatauv(L, sizeof(*g), 0);

    kosmos_gunzip_begin(&g->s);
    g->most = most > 0 ? (size_t)most : 0;
    luaL_setmetatable(L, GUNZIP_STREAM);

    return 1;
}

static int l_gunzip_feed(lua_State *L)
{
    struct gunzip_lua *g = luaL_checkudata(L, 1, GUNZIP_STREAM);
    size_t len = 0;
    const uint8_t *src = (const uint8_t *)luaL_checklstring(L, 2, &len);
    luaL_Buffer b;
    struct gunzip_into into = { &b, g->most, g->s.out };
    int result;

    luaL_buffinit(L, &b);
    result = kosmos_gunzip_feed(&g->s, src, len, put_into, &into);
    luaL_pushresult(&b);

    if (result == GUNZIP_WHOLE) {
        return 1;
    }

    lua_pushnil(L);
    lua_pushstring(L, kosmos_gunzip_said(result));
    return 2;
}

static int l_gunzip_finish(lua_State *L)
{
    struct gunzip_lua *g = luaL_checkudata(L, 1, GUNZIP_STREAM);
    int result = kosmos_gunzip_end(&g->s);

    lua_pushboolean(L, result == GUNZIP_WHOLE);

    if (result == GUNZIP_WHOLE) {
        return 1;
    }

    lua_pushstring(L, kosmos_gunzip_said(result));
    return 2;
}

/*
 * The compression kit: `use("/Kosmos/Kits/compress")`.
 *
 * It was in `sys` for an evening, next to `pack` and `fnv1a`, and it did not
 * belong there. `sys` is the syscall boundary - what only the kernel can do
 * - and inflating a buffer is not a syscall. Putting it there made `sys` the
 * place anything C-shaped landed, which is how a system interface turns into
 * a junk drawer.
 *
 * A kit is the BeOS answer and this system already claims that lineage:
 * Interface Kit, Storage Kit, Media Kit, Translation Kit. A named, documented
 * library of things a program will want, reached through the namespace like
 * everything else - so a program that was not given `/Kosmos/Kits` has none, the
 * same way a program that was not given `/Kosmos/Libraries` has no libraries.
 */
/* `deflate.c`: the deflater, CRC-32 and a copy, into the same table; and
 * `base64.c`, bytes as text and back. */
void kosmos_compress_deflate(lua_State *L);
void kosmos_compress_base64(lua_State *L);

void kosmos_compress_kit(lua_State *L)
{
    lua_newtable(L);

    kosmos_compress_deflate(L);
    kosmos_compress_base64(L);

    lua_pushcfunction(L, l_inflate);
    lua_setfield(L, -2, "inflate");

    lua_pushcfunction(L, l_inflate_into);
    lua_setfield(L, -2, "inflate_into");

    lua_pushcfunction(L, l_inflated_size);
    lua_setfield(L, -2, "inflated_size");

    lua_pushcfunction(L, l_gunzip);
    lua_setfield(L, -2, "gunzip");

    if (luaL_newmetatable(L, GUNZIP_STREAM)) {
        static const luaL_Reg stream[] = {
            { "feed",   l_gunzip_feed },
            { "finish", l_gunzip_finish },
            { NULL, NULL }
        };

        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        luaL_setfuncs(L, stream, 0);
    }

    lua_pop(L, 1);

    lua_pushcfunction(L, l_gunzip_stream);
    lua_setfield(L, -2, "gunzip_stream");
}
