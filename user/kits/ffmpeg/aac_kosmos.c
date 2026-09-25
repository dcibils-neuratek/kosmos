/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The AAC Kit**: FFmpeg's AAC decoder behind a Lua face (`roadmap.md`
 * 4e), for a film's sound in `/lib/video.lua`.
 *
 *   local aac = use("/kits/aac")
 *   local d, why = aac.decoder(config, rate, channels)
 *                             -- the MP4's AudioSpecificConfig, and the
 *                             -- rate and channels its sample entry states
 *   local pcm, rate, channels = d:decode(at, length)
 *                             -- one frame from memory at `at`: sixteen-bit
 *                             -- PCM, mono or stereo, as a string; "" when
 *                             -- the frame made none yet; nil and why when
 *                             -- it would not decode
 *   d:reset()                 -- a seek: forget the frame before
 *   d:close()
 *
 * **The shape of `/kits/mp3`'s `decode`, on purpose**: PCM and the rate
 * and channel count it is in, and the conversion to the device left to
 * `sys.pcm`, which already answers that question for every format. More
 * than two channels are mixed to two here, because only the decoder knows
 * which is which (`aac_core.h`).
 *
 * A frame in by address, like H.264's samples, and PCM out as a string,
 * like the MP3 Kit's: four kilobytes a frame, which is the music path's
 * shape today, and the same question for both when it is asked
 * (`roadmap.md` 5e).
 */

#include <stdint.h>

#include "lua.h"
#include "lauxlib.h"

#include "aac_core.h"

#define AAC_MT "kosmos.aac"

struct decoder {
    struct aac *d;
    int16_t     pcm[AAC_ROOM];
};

static struct decoder *check(lua_State *L)
{
    struct decoder *dec = (struct decoder *)luaL_checkudata(L, 1, AAC_MT);

    if (dec->d == NULL) {
        luaL_error(L, "this decoder has been closed");
    }

    return dec;
}

static int l_decoder(lua_State *L)
{
    size_t n;
    const char *config = luaL_checklstring(L, 1, &n);
    lua_Integer rate = luaL_optinteger(L, 2, 0);
    lua_Integer channels = luaL_optinteger(L, 3, 0);
    const char *why = NULL;
    struct decoder *dec;

    dec = (struct decoder *)lua_newuserdatauv(L, sizeof *dec, 0);
    dec->d = NULL;
    luaL_setmetatable(L, AAC_MT);

    dec->d = aac_open((const uint8_t *)config, n,
                      rate > 0 ? (unsigned)rate : 0u,
                      channels > 0 ? (unsigned)channels : 0u, &why);

    if (dec->d == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    }

    return 1;
}

static int l_decode(lua_State *L)
{
    struct decoder *dec = check(L);
    lua_Integer at = luaL_checkinteger(L, 2);
    lua_Integer n = luaL_checkinteger(L, 3);
    unsigned samples = 0, channels = 0, rate = 0;

    if (at == 0 || n <= 0) {
        return luaL_error(L, "a frame is an address and a length");
    }

    if (aac_decode(dec->d, (const uint8_t *)(uintptr_t)at, (size_t)n,
                   dec->pcm, AAC_ROOM, 1, &samples, &channels, &rate) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, aac_why(dec->d));
        return 2;
    }

    lua_pushlstring(L, (const char *)dec->pcm,
                    (size_t)samples * channels * sizeof(int16_t));
    lua_pushinteger(L, (lua_Integer)rate);
    lua_pushinteger(L, (lua_Integer)channels);
    return 3;
}

static int l_reset(lua_State *L)
{
    aac_reset(check(L)->d);
    return 0;
}

/* Also the collector's, so a decoder nobody closed is still given back. */
static int l_close(lua_State *L)
{
    struct decoder *dec = (struct decoder *)luaL_checkudata(L, 1, AAC_MT);

    aac_close(dec->d);
    dec->d = NULL;
    return 0;
}

void kosmos_aac_kit(lua_State *L)
{
    static const luaL_Reg methods[] = {
        { "decode", l_decode },
        { "reset",  l_reset },
        { "close",  l_close },
        { NULL, NULL }
    };

    static const luaL_Reg api[] = {
        { "decoder", l_decoder },
        { NULL, NULL }
    };

    if (luaL_newmetatable(L, AAC_MT)) {
        luaL_newlib(L, methods);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_close);
        lua_setfield(L, -2, "__gc");
    }

    lua_pop(L, 1);
    luaL_newlib(L, api);
}
