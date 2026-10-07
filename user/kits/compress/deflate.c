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
 * **Of miniz, the deflater is used here and the inflater, `tinfl`, in
 * `gzip.c`** - the kit's only inflater, for gzip, zlib, PNG, PDF and zip
 * alike. miniz reads and writes whole zip archives too, but through
 * `malloc` and `stdio`, and neither is how this system moves a file: a
 * file's bytes are read into a region (`fs.read_into`) and written from one
 * (`fs.write_from`), and a process's heap is 2 MB. So miniz is built with
 * no stdio, no time, no archive code and no allocator, and `tdefl` is run
 * over two regions the caller mapped, with its own state in pages of this
 * process's - about 320 KB, mapped the first time and kept.
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
        size_t pages = 0;

        state = kosmos_map_bytes(sizeof(tdefl_compressor), &pages);
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

/*
 * **A zlib stream that goes on** (`roadmap.md`, remote 7c):
 * `compress.zstream([level])`, and on it `z:deflate(src, bytes)` or
 * `z:deflate(string)` - what it was given, deflated and flushed to a byte
 * boundary, as a string.
 *
 * VNC's ZRLE is one zlib stream for a whole connection: two bytes of header
 * at its start and never an end, each update's tiles deflated on from the
 * last with the dictionary the stream has built, and flushed (`Z_SYNC_FLUSH`)
 * so the viewer can decode everything sent so far. So the deflater's state
 * belongs to the stream rather than to the kit, as `deflate_into`'s does:
 * about 320 KB a stream, mapped when it is made and given back when it is
 * collected. What comes out is small - a desktop's tiles are a few per cent
 * of its pixels before this - so it is handed back as a string rather than
 * into a region.
 */
struct zstream {
    tdefl_compressor *d;
    size_t pages;
};

#define ZSTREAM "kosmos.zstream"

static int l_zstream(lua_State *L)
{
    int level = (int)luaL_optinteger(L, 1, 6);
    struct zstream *z;
    mz_uint flags;

    if (level < 0 || level > 9) {
        return luaL_error(L, "zstream: a level is 0 to 9, not %d", level);
    }

    z = lua_newuserdatauv(L, sizeof *z, 0);
    z->d = NULL;
    z->pages = 0;
    luaL_setmetatable(L, ZSTREAM);

    z->d = kosmos_map_bytes(sizeof(tdefl_compressor), &z->pages);

    if (z->d == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "no memory for a deflater");
        return 2;
    }

    /* A positive window: the zlib header, before the first block. */
    flags = tdefl_create_comp_flags_from_zip_params(level, MZ_DEFAULT_WINDOW_BITS,
                                                    MZ_DEFAULT_STRATEGY);

    if (tdefl_init(z->d, NULL, NULL, (int)flags) != TDEFL_STATUS_OKAY) {
        return luaL_error(L, "zstream: the deflater would not start");
    }

    return 1;
}

static int l_zstream_deflate(lua_State *L)
{
    static uint8_t chunk[16384];
    struct zstream *z = luaL_checkudata(L, 1, ZSTREAM);
    const uint8_t *in;
    size_t left;
    luaL_Buffer b;
    tdefl_status st;

    if (lua_type(L, 2) == LUA_TSTRING) {
        in = (const uint8_t *)lua_tolstring(L, 2, &left);
    } else {
        in = (const uint8_t *)(uintptr_t)luaL_checkinteger(L, 2);
        left = (size_t)luaL_checkinteger(L, 3);

        if (in == NULL && left > 0) {
            return luaL_error(L, "zstream: needs a mapped region");
        }
    }

    if (z->d == NULL) {
        return luaL_error(L, "zstream: this stream has no deflater");
    }

    luaL_buffinit(L, &b);

    /* Until all of it is taken and the flush is out: a full chunk may mean
     * more is waiting, and asking again with nothing costs nothing. */
    for (;;) {
        size_t in_size = left, out_size = sizeof chunk;

        st = tdefl_compress(z->d, in, &in_size, chunk, &out_size, TDEFL_SYNC_FLUSH);

        if (st != TDEFL_STATUS_OKAY) {
            return luaL_error(L, "zstream: would not deflate (%d)", (int)st);
        }

        in += in_size;
        left -= in_size;
        luaL_addlstring(&b, (const char *)chunk, out_size);

        if (left == 0 && out_size < sizeof chunk) {
            break;
        }
    }

    luaL_pushresult(&b);
    return 1;
}

static int l_zstream_gc(lua_State *L)
{
    struct zstream *z = luaL_checkudata(L, 1, ZSTREAM);

    if (z->d != NULL) {
        kosmos_unmap((uintptr_t)z->d, z->pages);
        z->d = NULL;
    }

    return 0;
}

void kosmos_compress_deflate(lua_State *L)
{
    if (luaL_newmetatable(L, ZSTREAM)) {
        static const luaL_Reg methods[] = {
            { "deflate", l_zstream_deflate },
            { NULL, NULL },
        };

        luaL_newlib(L, methods);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_zstream_gc);
        lua_setfield(L, -2, "__gc");
    }

    lua_pop(L, 1);

    lua_pushcfunction(L, l_zstream);
    lua_setfield(L, -2, "zstream");

    lua_pushcfunction(L, l_deflate_into);
    lua_setfield(L, -2, "deflate_into");

    lua_pushcfunction(L, l_crc32);
    lua_setfield(L, -2, "crc32");

    lua_pushcfunction(L, l_adler32);
    lua_setfield(L, -2, "adler32");

    lua_pushcfunction(L, l_copy_into);
    lua_setfield(L, -2, "copy_into");
}
