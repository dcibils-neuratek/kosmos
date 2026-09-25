/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The H.264 Kit**: FFmpeg's decoder behind a Lua face (`roadmap.md` 4e),
 * for `/lib/video.lua` and anything else that has a film's samples.
 *
 *   local h264 = use("/kits/h264")
 *   local d, why = h264.decoder(avcc)   -- the MP4's avcC box, as a string
 *   d:send(at, length, pts)   -- one sample from memory at `at`, in decoding
 *                             -- order: true, or false and "full" (take a
 *                             -- picture first), or false and why
 *   d:picture(surface)        -- the next picture to show, drawn onto the
 *                             -- surface: its pts, width and height; or nil
 *                             -- and "none" (send more), "end", or why
 *   d:finish()                -- no more samples: the held pictures come out
 *   d:flush()                 -- forget everything, for a seek; the next
 *                             -- sample sent has to be a key frame
 *   d:close()
 *
 * **Samples by address and pictures into a surface**, never through a Lua
 * string: a film is thirty frames a second of both, and a string each way
 * would be the collector in the frame path - the audio server's mistake,
 * which `gfx.jpeg` taking `(at, length)` exists to avoid. `at` is where a
 * read buffer the caller mapped holds the sample; the pixels are converted
 * from FFmpeg's planes straight onto the surface by `gfx_draw_i420`.
 *
 * `h264_core.c` is the libavcodec side, and the Mac tests it without Lua.
 */

#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "../gfx/gfx_draw.h"
#include "h264_core.h"

#define H264_MT    "kosmos.h264"
#define SURFACE_MT "kosmos.surface"

struct decoder {
    struct h264 *d;
};

static struct decoder *check(lua_State *L)
{
    struct decoder *dec = (struct decoder *)luaL_checkudata(L, 1, H264_MT);

    if (dec->d == NULL) {
        luaL_error(L, "this decoder has been closed");
    }

    return dec;
}

static int l_decoder(lua_State *L)
{
    size_t n;
    const char *avcc = luaL_checklstring(L, 1, &n);
    const char *why = NULL;
    struct decoder *dec;

    dec = (struct decoder *)lua_newuserdatauv(L, sizeof *dec, 0);
    dec->d = NULL;
    luaL_setmetatable(L, H264_MT);

    dec->d = h264_open((const uint8_t *)avcc, n, &why);

    if (dec->d == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    }

    return 1;
}

static int l_send(lua_State *L)
{
    struct decoder *dec = check(L);
    lua_Integer at = luaL_checkinteger(L, 2);
    lua_Integer n = luaL_checkinteger(L, 3);
    lua_Integer pts = luaL_optinteger(L, 4, 0);

    if (at == 0 || n <= 0) {
        return luaL_error(L, "a sample is an address and a length");
    }

    switch (h264_send(dec->d, (const uint8_t *)(uintptr_t)at, (size_t)n,
                      (int64_t)pts)) {
    case H264_OK:
        lua_pushboolean(L, 1);
        return 1;
    case H264_AGAIN:
        lua_pushboolean(L, 0);
        lua_pushliteral(L, "full");
        return 2;
    default:
        lua_pushboolean(L, 0);
        lua_pushstring(L, h264_why(dec->d));
        return 2;
    }
}

static int l_picture(lua_State *L)
{
    struct decoder *dec = check(L);
    struct surface *s = (struct surface *)luaL_checkudata(L, 2, SURFACE_MT);
    struct h264_picture p;

    switch (h264_receive(dec->d, &p)) {
    case H264_OK:
        gfx_draw_i420(s, p.plane, p.stride, p.width, p.height, p.bt709,
                      p.full_range);
        lua_pushinteger(L, (lua_Integer)p.pts);
        lua_pushinteger(L, (lua_Integer)p.width);
        lua_pushinteger(L, (lua_Integer)p.height);
        return 3;
    case H264_AGAIN:
        lua_pushnil(L);
        lua_pushliteral(L, "none");
        return 2;
    case H264_END:
        lua_pushnil(L);
        lua_pushliteral(L, "end");
        return 2;
    default:
        lua_pushnil(L);
        lua_pushstring(L, h264_why(dec->d));
        return 2;
    }
}

static int l_finish(lua_State *L)
{
    struct decoder *dec = check(L);

    if (h264_finish(dec->d) != H264_OK) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, h264_why(dec->d));
        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int l_flush(lua_State *L)
{
    h264_flush(check(L)->d);
    return 0;
}

/* Also the collector's, so a decoder nobody closed is still given back. */
static int l_close(lua_State *L)
{
    struct decoder *dec = (struct decoder *)luaL_checkudata(L, 1, H264_MT);

    h264_close(dec->d);
    dec->d = NULL;
    return 0;
}

void kosmos_h264_kit(lua_State *L)
{
    static const luaL_Reg methods[] = {
        { "send",    l_send },
        { "picture", l_picture },
        { "finish",  l_finish },
        { "flush",   l_flush },
        { "close",   l_close },
        { NULL, NULL }
    };

    static const luaL_Reg api[] = {
        { "decoder", l_decoder },
        { NULL, NULL }
    };

    if (luaL_newmetatable(L, H264_MT)) {
        luaL_newlib(L, methods);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_close);
        lua_setfield(L, -2, "__gc");
    }

    lua_pop(L, 1);
    luaL_newlib(L, api);
}
