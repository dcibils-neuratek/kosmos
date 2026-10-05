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

static const char digits[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*
 * `compress.base64(bytes)` - the standard alphabet, padded with `=` as
 * RFC 4648 says, so any program's reader takes it: a mesh's points and
 * triangles saved as the text of a `data:` URI, or a file sent over a
 * Telnet line.
 */
static int l_base64(lua_State *L)
{
    size_t len, i, o = 0;
    const unsigned char *in = (const unsigned char *)luaL_checklstring(L, 1, &len);
    size_t want = (len + 2) / 3 * 4;
    luaL_Buffer b;
    char *out = luaL_buffinitsize(L, &b, want);

    for (i = 0; i + 2 < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];

        out[o++] = digits[v >> 18];
        out[o++] = digits[(v >> 12) & 63];
        out[o++] = digits[(v >> 6) & 63];
        out[o++] = digits[v & 63];
    }

    if (len - i == 1) {
        uint32_t v = (uint32_t)in[i] << 16;

        out[o++] = digits[v >> 18];
        out[o++] = digits[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (len - i == 2) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8;

        out[o++] = digits[v >> 18];
        out[o++] = digits[(v >> 12) & 63];
        out[o++] = digits[(v >> 6) & 63];
        out[o++] = '=';
    }

    luaL_pushresultsize(&b, o);
    return 1;
}

/*
 * `compress.unbase64(text)` - the bytes back. Decoding stops at the first
 * `=`, and anything else that is not base64 - a space, a line's end - is
 * refused rather than skipped, by where it is: text that is not what it
 * says is worth hearing about.
 */
static int l_unbase64(lua_State *L)
{
    size_t len, i, o = 0;
    const unsigned char *in = (const unsigned char *)luaL_checklstring(L, 1, &len);
    luaL_Buffer b;
    char *out = luaL_buffinitsize(L, &b, len / 4 * 3 + 3);
    uint32_t acc = 0;
    int bits = 0;

    for (i = 0; i < len; i++) {
        unsigned c = in[i], v;

        if (c >= 'A' && c <= 'Z') {
            v = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            v = c - 'a' + 26;
        } else if (c >= '0' && c <= '9') {
            v = c - '0' + 52;
        } else if (c == '+') {
            v = 62;
        } else if (c == '/') {
            v = 63;
        } else if (c == '=') {
            break;
        } else {
            return luaL_error(L, "not base64 at byte %d", (int)i);
        }

        acc = (acc << 6) | v;
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            out[o++] = (char)((acc >> bits) & 0xff);
        }
    }

    luaL_pushresultsize(&b, o);
    return 1;
}

void kosmos_compress_base64(lua_State *L)
{
    lua_pushcfunction(L, l_base64);
    lua_setfield(L, -2, "base64");

    lua_pushcfunction(L, l_unbase64);
    lua_setfield(L, -2, "unbase64");
}
