/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Flate, for whoever asks: `sys.inflate(bytes) -> string`.
 *
 * `png.c` has inflated its own image data since M6, because a PNG's pixels
 * arrive compressed and a decoder that cannot decompress them is not a
 * decoder. A PDF wants exactly the same thing for a different reason - 292
 * of the 294 streams in the document this was written against are
 * FlateDecode - and the answer should not be a second copy of the call.
 *
 * So the deflate that was private to the image decoder becomes something
 * any process can ask for. `puff.c` is the implementation and it is
 * vendored unmodified; this file is the twenty lines that make it reachable
 * from Lua.
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
#include "puff.h"

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
 * The way past it is the one already taken for large files: inflate into a
 * shared region the caller owns and hand back a capability, not bytes. That
 * is `read(fd, buf, n)` with a decoder in front of it, and it is worth
 * building the day something needs it. A page's content stream is a few
 * kilobytes; nothing yet does.
 */
#define INFLATE_MAX (1024u * 1024u)

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

/*
 * A zlib stream is a two-byte header and then raw deflate, and `puff` wants
 * the deflate. The header is checked rather than assumed away: the low
 * nibble of the first byte is the compression method and must be 8, and the
 * two bytes together are a multiple of 31.
 *
 * When it does not look like zlib the bytes are treated as raw deflate
 * instead of refused. That is not tidiness, it is a real case - producers
 * exist that write a bare deflate stream and call it FlateDecode - and
 * guessing wrong here costs nothing, because puff will refuse data that is
 * neither.
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

    unsigned long  destlen = 0;
    unsigned long  srclen;
    unsigned char *dest;
    size_t         pages = 0;
    int            err;

    source = skip_zlib_header(source, &source_len);

    if (source_len == 0) {
        return luaL_error(L, "inflate: no compressed data");
    }

    /*
     * Twice, on purpose. `puff` with no destination computes the size and
     * writes nothing, which is the only honest way to learn how much a
     * stream expands to: the alternative is guessing a multiple, growing on
     * failure, and inflating the beginning repeatedly. The first pass costs
     * the Huffman decode without the copies, and it means the buffer is
     * exact rather than nearly right.
     */
    srclen = (unsigned long)source_len;

    err = puff(NIL, &destlen, source, &srclen);
    if (err != 0) {
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

    dest = map_bytes((size_t)destlen, &pages);
    if (dest == NULL) {
        return luaL_error(L, "inflate: no room for %d bytes", (int)destlen);
    }

    srclen = (unsigned long)source_len;

    err = puff(dest, &destlen, source, &srclen);
    if (err != 0) {
        kosmos_unmap((uintptr_t)dest, pages);
        return luaL_error(L, "inflate: the second pass disagreed (%d)", err);
    }

    lua_pushlstring(L, (const char *)dest, (size_t)destlen);

    /* The string is a copy on the Lua heap, so the pages go back now. */
    kosmos_unmap((uintptr_t)dest, pages);

    return 1;
}

/*
 * `sys.inflate_into(src, src_bytes, dst, dst_bytes) -> written`
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

    const unsigned char *source;
    unsigned long        destlen = 0;
    unsigned long        srclen;
    int                  err;

    if (src == 0 || dst == 0) {
        return luaL_error(L, "inflate_into: needs two mapped regions");
    }

    if (src_len == 0) {
        return luaL_error(L, "inflate_into: no compressed data");
    }

    source = skip_zlib_header((const unsigned char *)src, &src_len);
    srclen = (unsigned long)src_len;

    err = puff(NIL, &destlen, source, &srclen);
    if (err != 0) {
        return luaL_error(L, "inflate_into: would not inflate (%d)", err);
    }

    if (destlen > dst_cap) {
        return luaL_error(L,
            "inflate_into: %d bytes will not fit in %d",
            (int)destlen, (int)dst_cap);
    }

    srclen = (unsigned long)src_len;

    err = puff((unsigned char *)dst, &destlen, source, &srclen);
    if (err != 0) {
        return luaL_error(L, "inflate_into: second pass disagreed (%d)", err);
    }

    lua_pushinteger(L, (lua_Integer)destlen);
    return 1;
}

/*
 * `sys.inflated_size(src, src_bytes) -> bytes`
 *
 * How big the result will be, without producing it. A caller needs this to
 * know whether the region it has is large enough, and asking costs the
 * Huffman decode without any of the copying.
 */
static int l_inflated_size(lua_State *L)
{
    uintptr_t src     = (uintptr_t)luaL_checkinteger(L, 1);
    size_t    src_len = (size_t)luaL_checkinteger(L, 2);

    const unsigned char *source;
    unsigned long        destlen = 0;
    unsigned long        srclen;
    int                  err;

    if (src == 0 || src_len == 0) {
        return luaL_error(L, "inflated_size: nothing to measure");
    }

    source = skip_zlib_header((const unsigned char *)src, &src_len);
    srclen = (unsigned long)src_len;

    err = puff(NIL, &destlen, source, &srclen);
    if (err != 0) {
        return luaL_error(L, "inflated_size: would not inflate (%d)", err);
    }

    lua_pushinteger(L, (lua_Integer)destlen);
    return 1;
}

/*
 * The compression kit: `use("/kits/compress")`.
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
 * everything else - so a program that was not given `/kits` has none, the
 * same way a program that was not given `/lib` has no libraries.
 */
void kosmos_compress_kit(lua_State *L)
{
    lua_newtable(L);

    lua_pushcfunction(L, l_inflate);
    lua_setfield(L, -2, "inflate");

    lua_pushcfunction(L, l_inflate_into);
    lua_setfield(L, -2, "inflate_into");

    lua_pushcfunction(L, l_inflated_size);
    lua_setfield(L, -2, "inflated_size");
}
