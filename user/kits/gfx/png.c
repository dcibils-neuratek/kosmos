/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * PNG, into a surface: `gfx.png`, the Lua door to the decoder.
 *
 * `gfx.png(bytes)` returns a `gfx.surface` holding the decoded image, in the
 * same 0xAARRGGBB every other surface uses, so everything that can draw a
 * surface can draw a picture without knowing where it came from.
 *
 * **The decoder is not here.** It is `gfx_png_decode_into` in
 * `picture_decode.c`, which no Lua state is needed to call - so a thread of
 * Flight's terrain builder decodes a tile with the same code this does
 * (`picture_decode.h`). What is left here is what only a Lua caller has:
 * reading the arguments, and the surface the pixels land in.
 */

#include <stddef.h>
#include <stdint.h>

#include "lua.h"
#include "lauxlib.h"

#include "gfx_draw.h"
#include "picture_decode.h"

/*
 * Where a decoded picture lands: a surface's own pages, mapped as
 * `gfx.surface` maps them, so the pixels go straight in at a surface's
 * pitch and nothing is copied a second time. The pages are kept for
 * `gfx_surface_new`, which hands them to the surface to own. `jpeg.c`
 * lands its pictures here too (`gfx_draw.h`).
 */
uint32_t *gfx_surface_room(void *user, unsigned w, unsigned h, unsigned *pitch)
{
    struct gfx_picture_landing *r = user;

    r->pixels = gfx_surface_map(w, h, &r->pitch, &r->pages);
    *pitch = r->pitch;
    return r->pixels;
}

int gfx_picture_push(lua_State *L, int result, struct gfx_picture_landing *r,
                     unsigned w, unsigned h, const char *format,
                     const char *why)
{
    if (result != GFX_PICTURE_OK) {
        /*
         * A PNG's wrong signature has always been said without the word in
         * front, since the sentence names the format itself; everything
         * else is "png: ..." or "jpeg: ...".
         */
        if (result == GFX_PICTURE_NOT_MINE && format[0] == 'p') {
            return luaL_error(L, "%s", why);
        }

        return luaL_error(L, "%s: %s", format, why ? why : "malformed");
    }

    /* Made by `gfx.c`, which is the only file that knows what one is. */
    gfx_surface_new(L, r->pixels, w, h, r->pitch, r->pages);
    return 1;
}

/*
 * `gfx.png(bytes)` or `gfx.png(address, length)`.
 *
 * **The second form exists because a photograph should not be a Lua
 * string.** A 1024x768 wallpaper is about a megabyte of PNG, and reading it
 * with `fs.read` builds that megabyte on the heap the collector walks, for
 * bytes about to be thrown away. So the caller reads into a region with
 * `fs.read_into` and passes where it landed. Same rule the rest of this
 * system runs on: bytes that are really pixels travel in a region, and the
 * language only says where they are.
 */
static int l_png(lua_State *L)
{
    size_t length = 0;
    const unsigned char *data;
    struct gfx_picture_landing r = { NULL, 0, 0 };
    unsigned w = 0, h = 0;
    const char *why = NULL;
    int result;

    if (lua_type(L, 1) == LUA_TNUMBER) {
        data = (const unsigned char *)(uintptr_t)luaL_checkinteger(L, 1);
        length = (size_t)luaL_checkinteger(L, 2);
    } else {
        data = (const unsigned char *)luaL_checklstring(L, 1, &length);
    }

    result = gfx_png_decode_into(data, length, gfx_surface_room, &r,
                                 &w, &h, &why);
    return gfx_picture_push(L, result, &r, w, h, "png", why);
}

void kosmos_png_open(lua_State *L)
{
    /* Into the `gfx` table, which is on the stack. */
    lua_pushcfunction(L, l_png);
    lua_setfield(L, -2, "png");
}
