/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Deflate, and the two other loops over bytes a zip needs:
 * `compress.deflate_into`, `compress.crc32` and `compress.copy_into`
 * (`roadmap.md` 6v).
 *
 * The compress kit could only inflate - `inflate.c`, for PNG and PDF - so a
 * zip could be read and never written. Writing one needs a compressor, and
 * Diego chose on 27 September to vendor miniz's rather than write one here
 * (`docs/rightclick.html`, answer 4): `tdefl`, the deflater at the heart of
 * miniz, which `runtime/upstream/miniz/` carries as released.
 *
 * **Only the deflater is used.** miniz reads and writes whole zip archives
 * too, but through `malloc` and `stdio`, and neither is how this system
 * moves a file: a file's bytes are read into a region (`fs.read_into`) and
 * written from one (`fs.write_from`), and a process's heap is 2 MB. So miniz
 * is built with no stdio, no time, no archive code and no allocator, and
 * `tdefl` is run over two regions the caller mapped, with its own state in
 * pages of this process's - about 320 KB, mapped the first time and kept.
 * The zip's structure - its headers and its directory - is `zip.lua`'s: a
 * few dozen bytes an entry, and a decision about each, which is Lua's side
 * of the line.
 *
 * **Raw DEFLATE**, not a zlib stream: a zip's method 8 is deflate with no
 * header and no Adler checksum, and a zip checks its data with CRC-32
 * instead - `crc32` below, miniz's own.
 *
 * Addresses come from `sys.memory_map`, as `inflate_into`'s do, and a wrong
 * one faults this process and nothing else.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "kosmos.h"
#include "miniz.h"

/* The deflater's state: its dictionary, its hash chains, its output. */
static tdefl_compressor *state;

static tdefl_compressor *deflater(void)
{
    if (state == NULL) {
        size_t pages = (sizeof(tdefl_compressor) + KOSMOS_PAGE_SIZE - 1)
                       / KOSMOS_PAGE_SIZE;
        long mapped = kosmos_map(pages);

        if (mapped >= 0) {
            state = (tdefl_compressor *)(uintptr_t)mapped;
        }
    }

    return state;
}

/*
 * `compress.deflate_into(src, bytes, dst, cap[, level]) -> bytes`, or nil and
 * why when the result does not fit in `cap`.
 *
 * `level` is zlib's, 0 to 9, and 6 when it is not said. Nought is stored
 * blocks - the bytes as they are, in deflate's framing - which is what a zip
 * writer falls back to when a file does not get smaller: a JPEG or a film is
 * compressed already, and deflating it again only costs time.
 *
 * One call, the whole input: `tdefl` is told this is all there is
 * (`TDEFL_FINISH`), so a result that does not fit comes back as "not done"
 * rather than as a stream to be continued, and is said so.
 */
static int l_deflate_into(lua_State *L)
{
    uintptr_t src   = (uintptr_t)luaL_checkinteger(L, 1);
    size_t    bytes = (size_t)luaL_checkinteger(L, 2);
    uintptr_t dst   = (uintptr_t)luaL_checkinteger(L, 3);
    size_t    cap   = (size_t)luaL_checkinteger(L, 4);
    int       level = (int)luaL_optinteger(L, 5, 6);

    tdefl_compressor *d = deflater();
    size_t in_size = bytes;
    size_t out_size = cap;
    tdefl_status st;
    mz_uint flags;

    if (dst == 0 || (src == 0 && bytes > 0)) {
        return luaL_error(L, "deflate_into: needs two mapped regions");
    }

    if (level < 0 || level > 9) {
        return luaL_error(L, "deflate_into: a level is 0 to 9, not %d", level);
    }

    if (d == NULL) {
        return luaL_error(L, "deflate_into: no memory for the deflater");
    }

    /* A negative window: raw deflate, which is what a zip holds. */
    flags = tdefl_create_comp_flags_from_zip_params(level,
                                                    -MZ_DEFAULT_WINDOW_BITS,
                                                    MZ_DEFAULT_STRATEGY);

    if (tdefl_init(d, NULL, NULL, (int)flags) != TDEFL_STATUS_OKAY) {
        return luaL_error(L, "deflate_into: the deflater would not start");
    }

    st = tdefl_compress(d, (const void *)src, &in_size, (void *)dst,
                        &out_size, TDEFL_FINISH);

    if (st == TDEFL_STATUS_DONE) {
        lua_pushinteger(L, (lua_Integer)out_size);
        return 1;
    }

    if (st == TDEFL_STATUS_OKAY) {
        lua_pushnil(L);
        lua_pushfstring(L, "%d bytes do not deflate into %d", (int)bytes,
                        (int)cap);
        return 2;
    }

    return luaL_error(L, "deflate_into: would not deflate (%d)", (int)st);
}

/*
 * `compress.crc32(src, bytes[, crc]) -> crc`: the CRC-32 a zip keeps of each
 * file, continued from `crc` when one is given, so a file read in pieces is
 * checked as one.
 */
static int l_crc32(lua_State *L)
{
    uintptr_t   src   = (uintptr_t)luaL_checkinteger(L, 1);
    size_t      bytes = (size_t)luaL_checkinteger(L, 2);
    lua_Integer crc   = luaL_optinteger(L, 3, 0);

    if (src == 0 && bytes > 0) {
        return luaL_error(L, "crc32: needs a mapped region");
    }

    lua_pushinteger(L, (lua_Integer)mz_crc32((mz_ulong)(uint32_t)crc,
                                             (const mz_uint8 *)src, bytes));
    return 1;
}

/*
 * `compress.adler32(src, bytes[, adler]) -> adler`: the Adler-32 a zlib
 * stream ends with, continued from `adler` when one is given.
 *
 * A PDF's `FlateDecode` is a zlib stream - two bytes of header, the
 * deflate, and this checksum of what was deflated, big-endian - where a
 * zip's method 8 is the deflate alone (Kosmos Write's PDF, `docs/write.md`
 * W3). The header and the four bytes are the writer's; the sum over the
 * bytes is a loop, and here.
 */
static int l_adler32(lua_State *L)
{
    uintptr_t   src   = (uintptr_t)luaL_checkinteger(L, 1);
    size_t      bytes = (size_t)luaL_checkinteger(L, 2);
    lua_Integer sum   = luaL_optinteger(L, 3, 1);

    if (src == 0 && bytes > 0) {
        return luaL_error(L, "adler32: needs a mapped region");
    }

    lua_pushinteger(L, (lua_Integer)mz_adler32((mz_ulong)(uint32_t)sum,
                                               (const unsigned char *)src,
                                               bytes));
    return 1;
}

/*
 * `compress.copy_into(src, dst, bytes)`: bytes from one place in the regions
 * to another, as they are. A zip's stored entry is its file with nothing
 * done to it, and writing one out means putting those bytes at the start of
 * a region - `fs.write_from` takes a region and a count, not an offset -
 * without passing them through a Lua string on the way.
 */
static int l_copy_into(lua_State *L)
{
    uintptr_t src   = (uintptr_t)luaL_checkinteger(L, 1);
    uintptr_t dst   = (uintptr_t)luaL_checkinteger(L, 2);
    size_t    bytes = (size_t)luaL_checkinteger(L, 3);

    if (bytes > 0 && (src == 0 || dst == 0)) {
        return luaL_error(L, "copy_into: needs two mapped regions");
    }

    memmove((void *)dst, (const void *)src, bytes);
    return 0;
}

void kosmos_compress_deflate(lua_State *L)
{
    lua_pushcfunction(L, l_deflate_into);
    lua_setfield(L, -2, "deflate_into");

    lua_pushcfunction(L, l_crc32);
    lua_setfield(L, -2, "crc32");

    lua_pushcfunction(L, l_adler32);
    lua_setfield(L, -2, "adler32");

    lua_pushcfunction(L, l_copy_into);
    lua_setfield(L, -2, "copy_into");
}
