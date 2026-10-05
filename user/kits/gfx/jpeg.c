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
 * Why the decoder is vendored when the PNG one is written here.
 *
 * They are not the same size of problem. PNG is an inflate and five filters,
 * and `png.c` is a file you can read in an afternoon and be sure of. JPEG is
 * a discrete cosine transform, a Huffman decoder, four chroma layouts, a
 * progressive mode with its own scan structure, and restart markers - and a
 * decoder that half-supports a format is worse than one that says no, which
 * is `png.c`'s own argument turned around on this one.
 *
 * So `stb_image` does it, instantiated JPEG-only in `stb_impl.c` beside
 * `stb_truetype`, which is the precedent and the same author. It is public
 * domain or MIT at the recipient's choice, unmodified, with its licence
 * recorded in `LICENSE.stb` next to it and a note of the exact commit.
 *
 * **And it runs at EL0, in whoever asked to draw.** stb_image's own warning
 * about untrusted input applies here as it does to the fonts: a malformed
 * JPEG that gets past its bounds checks kills the process that opened it and
 * nothing else, because that process is behind an address space. That is the
 * microkernel doing its job rather than a reason to be careless - the
 * wallpaper on a disk somebody handed you is untrusted input.
 *
 * --------------------------------------------------------------------
 * Where the pixels live, twice.
 *
 * stb decodes into the heap, which on this system grows by asking the kernel
 * for pages, so a full-size picture is a request it can serve. The surface
 * it ends up in cannot be heap: `gfx.c`'s finaliser unmaps *pages*, so a
 * surface has to be a region. That means one copy from the one to the other,
 * and the copy is not waste - the rows have to be widened from three bytes a
 * pixel to four and moved onto a 64-byte-aligned pitch anyway, which is a
 * pass over every pixel whatever happens.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "kosmos.h"
#include "gfx_draw.h"
#include "stb_image.h"

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

    int width = 0, height = 0, channels = 0;
    unsigned char *rgba = NULL;

    uint32_t *pixels;
    unsigned pitch = 0;
    size_t pages = 0;
    int y, x;

    if (lua_type(L, 1) == LUA_TNUMBER) {
        data = (const unsigned char *)(uintptr_t)luaL_checkinteger(L, 1);
        length = (size_t)luaL_checkinteger(L, 2);
    } else {
        data = (const unsigned char *)luaL_checklstring(L, 1, &length);
    }

    if (length > (size_t)INT32_MAX) {
        return luaL_error(L, "jpeg: larger than this system will decode");
    }

    /*
     * Four channels asked for, so a grey JPEG and a colour one arrive in the
     * same layout and the loop below has one case. stb fills alpha with 255
     * for a format that has none, which JPEG never does.
     */
    rgba = stbi_load_from_memory(data, (int)length,
                                 &width, &height, &channels, 4);

    if (rgba == NULL) {
        const char *why = stbi_failure_reason();

        return luaL_error(L, "jpeg: %s", why ? why : "malformed");
    }

    /* The same ceiling `png.c` keeps, and for the same reason: a header can
     * claim any size at all, and the multiplication below is where that
     * becomes this process's problem. */
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        stbi_image_free(rgba);

        return luaL_error(L, "jpeg: larger than this system will decode");
    }

    /* And now a surface's pixels, exactly as gfx.surface maps them. */
    pixels = gfx_surface_map((unsigned)width, (unsigned)height, &pitch, &pages);

    if (pixels == NULL) {
        stbi_image_free(rgba);

        return luaL_error(L, "jpeg: no room for the picture");
    }

    /*
     * stb gives RGBA in memory order; a surface is 0xAARRGGBB in a word.
     * Written a component at a time rather than as a cast, because the two
     * agree only on a little-endian machine and this system is meant to
     * reach one that is not.
     */
    for (y = 0; y < height; y++) {
        const unsigned char *src = rgba + (size_t)y * (size_t)width * 4;
        uint32_t *dst = (uint32_t *)((unsigned char *)pixels
                                     + (size_t)y * pitch);

        for (x = 0; x < width; x++) {
            const unsigned char *p = src + (size_t)x * 4;

            dst[x] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16)
                   | ((uint32_t)p[1] << 8)  | (uint32_t)p[2];
        }
    }

    stbi_image_free(rgba);

    /* Made by `gfx.c`, which is the only file that knows what one is. */
    gfx_surface_new(L, pixels, (unsigned)width, (unsigned)height, pitch, pages);
    return 1;
}

void kosmos_jpeg_open(lua_State *L)
{
    /* Into the `gfx` table, which is on the stack. */
    lua_pushcfunction(L, l_jpeg);
    lua_setfield(L, -2, "jpeg");
}
