/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Base64, both ways: `compress.base64(bytes) -> text` and
 * `compress.unbase64(text) -> bytes`.
 *
 * **In the Compression Kit because it is general**, and that is the whole
 * reason it moved. It was `k3.base64` and `k3.unbase64`, in the 3D Kit,
 * because Cafesa3D was the first to want it - a glTF file keeps its meshes
 * as the text of a `data:` URI - and the next to want it, `telnetd`'s `get`
 * and `put`, had written its own in Lua rather than ask a 3D kit for a text
 * encoding. One door, in the kit that is about bytes in other shapes.
 *
 * A loop over bytes, so here rather than in Lua (`CLAUDE.md`, *Language
 * split*). Each is told its result's exact size first, so the buffer is
 * claimed once rather than grown a character at a time.
 */

#include <stddef.h>
#include <stdint.h>

#include "lua.h"
#include "lauxlib.h"

#include "base64_core.h"

static int l_base64(lua_State *L)
{
    size_t len;
    const unsigned char *in = (const unsigned char *)luaL_checklstring(L, 1, &len);
    luaL_Buffer b;
    char *out = luaL_buffinitsize(L, &b, (len + 2) / 3 * 4);

    luaL_pushresultsize(&b, base64_encode(in, len, out));
    return 1;
}

/*
 * `compress.unbase64(text)` - the bytes back. Decoding stops at the first
 * `=`, and anything else that is not base64 - a space, a line's end - is
 * refused rather than skipped, by where it is: text that is not what it
 * says is worth hearing about. (A message's base64, broken into lines, is
 * the Mail Kit's to undo, with `base64_decode`'s `lines`.)
 */
static int l_unbase64(lua_State *L)
{
    size_t len, bad, got;
    const char *in = luaL_checklstring(L, 1, &len);
    luaL_Buffer b;
    uint8_t *out = (uint8_t *)luaL_buffinitsize(L, &b, len / 4 * 3 + 3);

    got = base64_decode(in, len, out, len / 4 * 3 + 3, 0, &bad);

    if (bad < len) {
        return luaL_error(L, "not base64 at byte %d", (int)bad);
    }

    luaL_pushresultsize(&b, got);
    return 1;
}

void kosmos_compress_base64(lua_State *L)
{
    lua_pushcfunction(L, l_base64);
    lua_setfield(L, -2, "base64");

    lua_pushcfunction(L, l_unbase64);
    lua_setfield(L, -2, "unbase64");
}
