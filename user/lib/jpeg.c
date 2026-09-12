/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `gfx.jpeg(bytes)` returns a `gfx.surface` holding the decoded image, in
 * exactly the layout `gfx.png` returns and `gfx.surface` makes.
 *
 * --------------------------------------------------------------------
 * Why this is a function on `gfx` and not a kit.
 *
 * A kit is code you run, reached through the namespace - `use("/kits/pdf")`
 * - and a JPEG decoder has the shape of one: a loop over bytes, small and
 * bounded, exactly where C buys something. It was nearly written as one.
 *
 * What decided it was the caller. `picture_from_file` in the window manager
 * opens a picture; it should not have to know that one format lives on the
 * `gfx` table and another behind a `use`. A person adding a wallpaper is not
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
#include "stb_image.h"

/* Must match gfx.c and png.c. A surface created here is freed by gfx.c's
 * finaliser, so all three have to agree about what one is. */
#define SURFACE_MT  "kosmos.surface"
#define ROW_ALIGN   64

struct surface {
    uint32_t *pixels;
    unsigned  width;
    unsigned  height;
    unsigned  pitch;
    size_t    bytes;
    size_t    pages;
    bool      owned;
};

/*
 * Pages, for something too big for the heap. The same helper `png.c` has,
 * and duplicated rather than shared for the reason the two files are
 * separate at all: this one is a binding around somebody else's decoder and
 * that one is a decoder, and a header holding four lines so that they can
 * agree about `kosmos_map` would be a dependency for nothing.
 */
static void *map_pages(size_t bytes, size_t *pages_out)
{
    size_t pages = (bytes + KOSMOS_PAGE_SIZE - 1) / KOSMOS_PAGE_SIZE;
    long mapped;

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

    struct surface *s;
    uint32_t *pixels;
    size_t pitch, bytes, pages;
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

    /* And now a surface, exactly as gfx.surface makes one. */
    pitch = (((size_t)width * 4) + (ROW_ALIGN - 1)) & ~(size_t)(ROW_ALIGN - 1);
    bytes = pitch * (size_t)height;
    pixels = map_pages(bytes, &pages);

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

    s = lua_newuserdatauv(L, sizeof(*s), 0);
    s->pixels = pixels;
    s->width  = (unsigned)width;
    s->height = (unsigned)height;
    s->pitch  = (unsigned)pitch;
    s->bytes  = bytes;
    s->pages  = pages;
    s->owned  = true;

    luaL_setmetatable(L, SURFACE_MT);

    /* The collector sees a small userdata and megabytes behind it. Telling
     * it the real size is what makes the finaliser something other than
     * theoretical - gfx.md 19.6. */
    lua_gc(L, LUA_GCSTEP, (int)(bytes / 1024));

    return 1;
}

void kosmos_jpeg_open(lua_State *L)
{
    /* Into the `gfx` table, which is on the stack. */
    lua_pushcfunction(L, l_jpeg);
    lua_setfield(L, -2, "jpeg");
}
