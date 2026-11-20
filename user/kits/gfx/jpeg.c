/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `gfx.jpeg(bytes)` returns a `gfx.surface` holding the decoded image, in
 * exactly the layout `gfx.png` returns and `gfx.surface` makes.
 *
 * --------------------------------------------------------------------
 * Why this is a function on `gfx` and not a kit.
 *
 * A kit is code you run, reached through the namespace - `use("/Kosmos/Kits/pdf")`
 * - and a JPEG decoder has the shape of one: a loop over bytes, small and
 * bounded, exactly where C buys something. It was nearly written as one.
 *
 * What decided it was the caller. `decoder_for` in the window manager
 * chooses how a picture is opened; it should not have to know that one
 * format lives on the `gfx` table and another behind a `use`. A person adding a wallpaper is not
 * choosing a decoder, and the code that opens it should not read as though
 * they were. PNG has been `gfx.png` since it arrived, so JPEG is `gfx.jpeg`
 * and the two sit together where somebody looking for "how do I open a
 * picture" will find both.
 *
 * --------------------------------------------------------------------
 * The decoder is not here.
 *
 * It is `gfx_jpeg_decode_into` in `picture_decode.c` - stb_image, and the
 * conversion from its RGBA bytes to this system's 0xAARRGGBB words - which
 * no Lua state is needed to call, so a thread of Flight's terrain builder
 * decodes a tile with the same code this does (`picture_decode.h`). What
 * is left here is reading the arguments and the surface the pixels land
 * in, which `png.c` makes for both.
 */

#include <stddef.h>
#include <stdint.h>

#include "lua.h"
#include "lauxlib.h"

#include "gfx_draw.h"
#include "picture_decode.h"

/*
 * The bytes, either as a Lua string or as an address and a length.
 *
 * The second form is what the window manager uses: a wallpaper is read into
 * a region and decoded from it, so the file never becomes a Lua string. Four
 * megabytes of JPEG through the interpreter would be four megabytes the
 * collector has to walk, for bytes that are about to be thrown away - the
 * same bargain `gfx.png` makes and for the same reason.
 */
static int l_jpeg(lua_State *L)
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

    result = gfx_jpeg_decode_into(data, length, gfx_surface_room, &r,
                                  &w, &h, &why);
    return gfx_picture_push(L, result, &r, w, h, "jpeg", why);
}

void kosmos_jpeg_open(lua_State *L)
{
    /* Into the `gfx` table, which is on the stack. */
    lua_pushcfunction(L, l_jpeg);
    lua_setfield(L, -2, "jpeg");
}
