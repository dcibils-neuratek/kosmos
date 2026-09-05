/*
 * PNG, into a surface.
 *
 * `gfx.png(bytes)` returns a `gfx.surface` holding the decoded image, in the
 * same 0xAARRGGBB every other surface uses, so everything that can draw a
 * surface can draw a picture without knowing where it came from.
 *
 *--------------------------------------------------------------------------
 * Why this is C.
 *
 * `CLAUDE.md`'s test is whether a bug can escape: if it can corrupt another
 * process it is C's job to prevent, and if it can only kill its own process
 * it may be Lua. By that test alone this could be Lua - it runs at EL0 in
 * whichever application asked, and a bug in it kills that application.
 *
 * It is C for the other reason: it is a pixel loop. Decoding is per-byte
 * work over the whole image - unfiltering touches every channel of every
 * pixel and refers to the one to its left and the one above - and `gfx.md`
 * 19.2 is explicit that a Lua loop costs twenty to fifty nanoseconds an
 * iteration, which for a modest photograph is several seconds of nothing
 * happening.
 *
 * A pure-Lua decoder would also have to build the image as Lua values before
 * it became a surface, and `gfx.md` 19.1 forbids exactly that: a million
 * pixels in a table is a collector walking a million slots every cycle.
 *
 *--------------------------------------------------------------------------
 * What it handles, and what it refuses.
 *
 * Eight bits a channel, colour types 2 (RGB), 6 (RGBA) and 0 (greyscale),
 * no interlace. That covers what an encoder produces by default and what a
 * screenshot or a photograph is.
 *
 * It refuses the rest - sixteen-bit channels, palettes, Adam7 interlacing -
 * by saying which one it found, rather than by producing an image that is
 * subtly wrong. A decoder that half-supports a format is worse than one
 * that does not, because the failure arrives as a picture that looks like
 * somebody's bug.
 *
 *--------------------------------------------------------------------------
 * Memory.
 *
 * Two large buffers, and neither can come from the heap: a process's heap is
 * 2 MB by design, and a 1280x853 image needs about four and a half megabytes
 * for its pixels and as much again for the inflated rows. Both come from
 * `kosmos_map`, which is where `gfx.surface` gets its pixels and for the
 * same reason.
 *
 * The inflated buffer is handed back before this returns. Only the surface
 * survives, and that is the Lua object's to free.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "kosmos.h"
#include "puff.h"

/* Must match gfx.c. A surface created here is freed by gfx.c's finaliser, so
 * the two have to agree about what one is. */
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

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/*
 * Pages, for something too big for the heap. Returns NULL rather than
 * raising: the caller has other things mapped and has to hand them back.
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
 * The Paeth predictor, from the PNG specification, unchanged.
 *
 * Written out rather than shortened because it is the one filter whose
 * behaviour is not obvious from its name, and because getting the tie-break
 * wrong produces an image that is right almost everywhere - which is the
 * hardest kind of wrong to find.
 */
static unsigned char paeth(unsigned char a, unsigned char b, unsigned char c)
{
    int p = (int)a + (int)b - (int)c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;

    if (pa <= pb && pa <= pc) {
        return a;
    }

    return (pb <= pc) ? b : c;
}

/*
 * Undoes the per-row filter, in place, one scanline at a time.
 *
 * `bpp` is bytes per pixel and is what "the one to the left" means: the
 * filters refer to the corresponding byte of the previous *pixel*, not the
 * previous byte, and using one for the other gives a picture with the colour
 * channels smeared along each row.
 */
static bool unfilter(unsigned char *rows, unsigned height, size_t stride,
                     unsigned bpp)
{
    unsigned char *previous = NULL;
    unsigned y;

    for (y = 0; y < height; y++) {
        unsigned char *line = rows + (size_t)y * (stride + 1);
        unsigned char kind = line[0];
        unsigned char *data = line + 1;
        size_t i;

        for (i = 0; i < stride; i++) {
            unsigned char left = (i >= bpp) ? data[i - bpp] : 0;
            unsigned char up = previous ? previous[i] : 0;
            unsigned char upleft = (previous && i >= bpp) ? previous[i - bpp]
                                                          : 0;

            switch (kind) {
            case 0:                                     break;
            case 1: data[i] = (unsigned char)(data[i] + left);   break;
            case 2: data[i] = (unsigned char)(data[i] + up);     break;
            case 3: data[i] = (unsigned char)(data[i]
                                              + (((unsigned)left
                                                  + (unsigned)up) / 2)); break;
            case 4: data[i] = (unsigned char)(data[i]
                                              + paeth(left, up, upleft)); break;
            default:
                return false;                           /* not a filter */
            }
        }

        previous = data;
    }

    return true;
}

/*
 * `gfx.png(bytes)` or `gfx.png(address, length)`.
 *
 * **The second form exists because a photograph does not fit in a Lua
 * string.** The heap is 2 MB by design - `design.md` 5.2 wants a small one,
 * because a small heap collects fast and the collector's worst pause is what
 * decides whether the desktop stutters - and a 1024x768 wallpaper is about a
 * megabyte of PNG. Reading it with `fs.read` builds that megabyte as a Lua
 * string, out of chunks that are themselves on the heap, and the answer is
 * "cannot read" on a file that is plainly there.
 *
 * So the caller reads into a region with `fs.read_into` and passes where it
 * landed. Same rule the rest of this system runs on: bytes that are really
 * pixels travel in a region, and the language only says where they are.
 */
static int l_png(lua_State *L)
{
    size_t length = 0;
    const unsigned char *data;

    if (lua_type(L, 1) == LUA_TNUMBER) {
        data = (const unsigned char *)(uintptr_t)luaL_checkinteger(L, 1);
        length = (size_t)luaL_checkinteger(L, 2);
    } else {
        data = (const unsigned char *)luaL_checklstring(L, 1, &length);
    }

    unsigned char signature[8] = {
        0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
    };

    unsigned long width = 0, height = 0;
    unsigned depth = 0, colour = 0, interlace = 0;
    unsigned channels = 0, bpp = 0;
    size_t stride = 0;

    unsigned char *idat = NULL;
    size_t idat_len = 0, idat_cap = 0, idat_pages = 0;

    unsigned char *rows = NULL;
    size_t rows_pages = 0;
    unsigned long rows_len = 0;

    struct surface *s;
    uint32_t *pixels;
    size_t pitch, bytes, pages;
    size_t at;
    unsigned y, x;
    const char *why = NULL;

    if (length < 8 || memcmp(data, signature, 8) != 0) {
        return luaL_error(L, "that is not a PNG: the signature is wrong");
    }

    /*
     * Every chunk, in order. Length, type, body, CRC - and the CRC is not
     * checked, deliberately: it guards against a corrupted file and this
     * decoder's bounds checks guard against a hostile one, which is the
     * threat that matters when the bytes came from somewhere else.
     */
    at = 8;

    while (at + 8 <= length) {
        uint32_t size = be32(data + at);
        const unsigned char *type = data + at + 4;
        const unsigned char *body = data + at + 8;

        if (size > length || at + 12 + size > length) {
            why = "a chunk runs past the end of the file";
            goto done;
        }

        if (memcmp(type, "IHDR", 4) == 0) {
            if (size < 13) {
                why = "the header chunk is too short";
                goto done;
            }

            width     = be32(body);
            height    = be32(body + 4);
            depth     = body[8];
            colour    = body[9];
            interlace = body[12];
        } else if (memcmp(type, "IDAT", 4) == 0) {
            /*
             * The compressed data may be split across any number of chunks
             * and has to be inflated as one stream, so it is gathered first.
             * Sized once, from the file, because the total cannot exceed it.
             */
            if (idat == NULL) {
                idat = map_pages(length, &idat_pages);

                if (idat == NULL) {
                    why = "no room to gather the compressed data";
                    goto done;
                }

                idat_cap = idat_pages * KOSMOS_PAGE_SIZE;
            }

            if (idat_len + size > idat_cap) {
                why = "the compressed data is larger than the file";
                goto done;
            }

            memcpy(idat + idat_len, body, size);
            idat_len += size;
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }

        at += 12 + size;
    }

    if (width == 0 || height == 0) {
        why = "no header, or an image with no pixels in it";
        goto done;
    }

    if (width > 8192 || height > 8192) {
        why = "larger than this system will decode";
        goto done;
    }

    if (depth != 8) {
        why = "only eight bits a channel";
        goto done;
    }

    if (interlace != 0) {
        why = "interlaced, which this does not do";
        goto done;
    }

    switch (colour) {
    case 0: channels = 1; break;                /* grey */
    case 2: channels = 3; break;                /* RGB */
    case 6: channels = 4; break;                /* RGBA */
    default:
        why = "a colour type this does not do - a palette, or grey+alpha";
        goto done;
    }

    if (idat == NULL) {
        why = "no image data";
        goto done;
    }

    bpp = channels;                             /* eight bits a channel */
    stride = (size_t)width * channels;

    /*
     * One filter byte a row, which is why this is not width * channels *
     * height. Getting that wrong gives an inflate that succeeds and an image
     * that walks diagonally, which is a memorable afternoon.
     */
    rows_len = (unsigned long)((stride + 1) * height);
    rows = map_pages(rows_len, &rows_pages);

    if (rows == NULL) {
        why = "no room for the decoded rows";
        goto done;
    }

    if (puff(rows, &rows_len, idat + 2, &(unsigned long){ idat_len - 2 }) != 0) {
        why = "the compressed data would not inflate";
        goto done;
    }

    if (rows_len != (unsigned long)((stride + 1) * height)) {
        why = "the image is a different size than its header says";
        goto done;
    }

    if (!unfilter(rows, (unsigned)height, stride, bpp)) {
        why = "a row uses a filter that is not one of the five";
        goto done;
    }

    /* And now a surface, exactly as gfx.surface makes one. */
    pitch = (((size_t)width * 4) + (ROW_ALIGN - 1)) & ~(size_t)(ROW_ALIGN - 1);
    bytes = pitch * height;
    pixels = map_pages(bytes, &pages);

    if (pixels == NULL) {
        why = "no room for the picture";
        goto done;
    }

    for (y = 0; y < (unsigned)height; y++) {
        const unsigned char *src = rows + (size_t)y * (stride + 1) + 1;
        uint32_t *dst = (uint32_t *)((unsigned char *)pixels
                                     + (size_t)y * pitch);

        for (x = 0; x < (unsigned)width; x++) {
            const unsigned char *p = src + (size_t)x * channels;
            uint32_t r, g, b, a;

            if (channels == 1) {
                r = g = b = p[0];
                a = 255;
            } else {
                r = p[0];
                g = p[1];
                b = p[2];
                a = (channels == 4) ? p[3] : 255;
            }

            dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    kosmos_unmap((uintptr_t)rows, rows_pages);
    kosmos_unmap((uintptr_t)idat, idat_pages);

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

done:
    if (rows != NULL) {
        kosmos_unmap((uintptr_t)rows, rows_pages);
    }

    if (idat != NULL) {
        kosmos_unmap((uintptr_t)idat, idat_pages);
    }

    return luaL_error(L, "png: %s", why ? why : "malformed");
}

void kosmos_png_open(lua_State *L)
{
    /* Into the `gfx` table, which is on the stack. */
    lua_pushcfunction(L, l_png);
    lua_setfield(L, -2, "png");
}
