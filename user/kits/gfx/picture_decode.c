/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * PNG and JPEG into 0xAARRGGBB pixels, with no Lua in them
 * (`picture_decode.h` is the argument for the split; `png.c` and `jpeg.c`
 * are the Lua doors, `gfx.png` and `gfx.jpeg`).
 *
 *--------------------------------------------------------------------------
 * Why this is C.
 *
 * It is a pixel loop. Decoding is per-byte work over the whole image -
 * unfiltering touches every channel of every pixel and refers to the one to
 * its left and the one above - and `gfx.md` 19.2 is explicit that a Lua
 * loop costs twenty to fifty nanoseconds an iteration, which for a modest
 * photograph is several seconds of nothing happening. A pure-Lua decoder
 * would also have to build the image as Lua values before it became a
 * surface, and `gfx.md` 19.1 forbids exactly that.
 *
 *--------------------------------------------------------------------------
 * What the PNG half handles, and what it refuses.
 *
 * Eight bits a channel, colour types 2 (RGB), 6 (RGBA), 0 (greyscale) and
 * 3 (a palette, with its transparency), no interlace. That covers what an
 * encoder produces by default, what a screenshot or a photograph is, a
 * field of one colour, which every tool writes with a palette, and a
 * terrarium elevation tile, which is RGB.
 *
 * It refuses the rest - channels of other than eight bits, grey with alpha,
 * Adam7 interlacing - by saying which one it found, rather than by
 * producing an image that is subtly wrong. A decoder that half-supports a
 * format is worse than one that does not, because the failure arrives as a
 * picture that looks like somebody's bug.
 *
 *--------------------------------------------------------------------------
 * Why the JPEG half is vendored when the PNG one is written here.
 *
 * PNG is an inflate and five filters, a file you can read in an afternoon
 * and be sure of. JPEG is a discrete cosine transform, a Huffman decoder,
 * four chroma layouts, a progressive mode and restart markers - so
 * `stb_image` does it, instantiated JPEG-only in `stb_impl.c`, unmodified.
 * It runs in whoever asked, so a malformed JPEG that got past its bounds
 * checks kills that process and nothing else.
 *
 *--------------------------------------------------------------------------
 * Memory, and threads.
 *
 * Every buffer a decode uses is its own and comes from `malloc`: the PNG's
 * gathered IDAT, its inflated rows and the inflater's 43 KB of state, and
 * stb's own working memory for a JPEG. All of it is handed back before the
 * decode returns; only the pixels survive, in memory the caller's `room`
 * gave.
 *
 * **This used to be pages from `kosmos_map`, and the inflater the
 * process's one.** The pages were because a process's heap was once 2 MB
 * and a wallpaper's rows are megabytes; the heap grows since (`malloc.c`),
 * as stb's JPEG already relied on. The inflater was `kosmos_inflater()`, a
 * single workspace kept per process - correct while only the Lua thread
 * decoded, and a torn inflate the first time a terrain thread decoded a
 * tile while the window inflated anything at all. A workspace per decode
 * costs one 43 KB allocation against megabytes of rows, and removes the
 * last state two decodes shared.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bytes.h"
#include "picture_decode.h"
#include "kits/compress/gzip.h"
#include "stb_image.h"

/* The largest side either decoder takes. A header can claim any size at
 * all, and the multiplications below are where that becomes this
 * process's problem. */
#define PICTURE_MAX 8192u

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
 * Four of them at once, which is what a pixel is.
 *
 * `paeth` above works on one byte and each filter refers to the same channel
 * of the *previous pixel* - so the four channels of an RGBA pixel depend on
 * nothing of each other, and the serial chain is one pixel long rather than
 * one byte. That is the whole trick: still one step per pixel, but four
 * lanes wide.
 *
 * Bytes in a vector rather than the `int` arithmetic above, so the
 * subtraction has to be done where it cannot wrap. Sixteen-bit lanes hold
 * `a + b - c` for any bytes, which is -255 to 510.
 */
typedef short i16x4 __attribute__((vector_size(8)));

static inline i16x4 paeth4(i16x4 a, i16x4 b, i16x4 c)
{
    i16x4 p  = a + b - c;
    i16x4 pa = p - a, pb = p - b, pc = p - c;
    i16x4 wins_a, wins_b;

    /*
     * Absolute value without a branch, and without a ternary: C has no
     * vector `?:`, so the sign is made into a mask. An arithmetic shift of
     * a signed lane by 15 is all ones when negative and all zeros when not,
     * and `(x ^ s) - s` is then `-x` or `x`.
     */
    pa = (pa ^ (pa >> 15)) - (pa >> 15);
    pb = (pb ^ (pb >> 15)) - (pb >> 15);
    pc = (pc ^ (pc >> 15)) - (pc >> 15);

    /*
     * The specification's tie-break, as masks for the same reason: `&&` is
     * not a vector operation either. A comparison yields all ones where it
     * holds, so the three cases can be selected and combined.
     *
     * Getting this wrong gives an image that is right almost everywhere,
     * which is the hardest kind of wrong to find - so it is written as the
     * specification words it: `a` wins ties against both, `b` wins ties
     * against `c`.
     */
    wins_a = (pa <= pb) & (pa <= pc);
    wins_b = ~wins_a & (pb <= pc);

    return (a & wins_a) | (b & wins_b) | (c & ~(wins_a | wins_b));
}

static inline i16x4 load4(const unsigned char *p)
{
    return (i16x4){ p[0], p[1], p[2], p[3] };
}

static inline void store4(unsigned char *p, i16x4 v)
{
    p[0] = (unsigned char)v[0];
    p[1] = (unsigned char)v[1];
    p[2] = (unsigned char)v[2];
    p[3] = (unsigned char)v[3];
}

/*
 * Undoes the per-row filter, in place, one scanline at a time.
 *
 * `bpp` is bytes per pixel and is what "the one to the left" means: the
 * filters refer to the corresponding byte of the previous *pixel*, not the
 * previous byte, and using one for the other gives a picture with the colour
 * channels smeared along each row.
 *
 *--------------------------------------------------------------------------
 * **The filter is chosen once a row, and used to be tested once a byte.**
 *
 * This was one loop with a `switch (kind)` inside it, plus `i >= bpp` and
 * `previous ? ...` re-evaluated for every byte - all three decided before
 * the row began. On a 1920x1080 wallpaper that is 8.3 million iterations
 * paying for a decision made once, and it cost more than the filters did.
 * Measured on real cores, per wallpaper:
 *
 *     Sub       8.09 -> 5.50 ms
 *     Up        7.46 -> 1.91 -> 0.15 ms vectorised
 *     Average   7.54 -> 6.63 ms
 *     Paeth    12.05 -> 11.92 ms
 *
 * So: a loop per filter, the first pixel peeled off so the rest needs no
 * bounds test, and vectors where the dependency allows. **`Up` is fifty
 * times quicker** - it depends only on the row above, so a whole row goes
 * sixteen bytes at a time. Paeth is the one hoisting could not help, because
 * there the cost really is the arithmetic, and that is what `paeth4` is for.
 *
 * `previous` points at a row of zeros for the first line rather than being
 * NULL, which removes the other per-byte test: the specification says the
 * row above the first is zero, so saying that once is both faster and more
 * literally what the format states.
 */
static bool unfilter(unsigned char *rows, unsigned height, size_t stride,
                     unsigned bpp)
{
    static const unsigned char zeros[16] = { 0 };
    const unsigned char *previous = zeros;
    unsigned y;

    /*
     * `zeros` stands in for the row above the first, and is only ever read
     * for the first `bpp` bytes - every filter that needs a whole row above
     * has its own first-row case below. The guard is on `bpp` for that
     * reason, and a stride wider than `zeros` is not merely allowed but
     * usual.
     *
     * This was wrong and a host checker found it: the general cases read
     * `previous[i]` across the whole row, which on line zero walked off the
     * end of a sixteen-byte array. 213,531 bytes of a million differed -
     * "right almost everywhere", which is what the comment on `paeth` warns
     * this file is prone to.
     */
    if (bpp == 0 || bpp > sizeof zeros) {
        return false;
    }

    for (y = 0; y < height; y++) {
        unsigned char *line = rows + (size_t)y * (stride + 1);
        unsigned char kind = line[0];
        unsigned char *data = line + 1;
        bool first = (y == 0);
        size_t i;

        switch (kind) {
        case 0:                                 /* none */
            break;

        case 1:                                 /* Sub: the pixel to the left */
            for (i = bpp; i < stride; i++) {
                data[i] = (unsigned char)(data[i] + data[i - bpp]);
            }
            break;

        case 2:                                 /* Up: the row above */
            if (first) {
                break;                          /* adding zero */
            }

            if (bpp == 4) {
                /* `__builtin_memcpy`, not `memcpy`: under -ffreestanding
                 * GCC called the libc for each (`rows.c` says more). */
                typedef unsigned char u8x16 __attribute__((vector_size(16)));

                for (i = 0; i + 16 <= stride; i += 16) {
                    u8x16 d, u;

                    __builtin_memcpy(&d, data + i, sizeof d);
                    __builtin_memcpy(&u, previous + i, sizeof u);
                    d = d + u;
                    __builtin_memcpy(data + i, &d, sizeof d);
                }
            } else {
                i = 0;
            }

            for (; i < stride; i++) {
                data[i] = (unsigned char)(data[i] + previous[i]);
            }
            break;

        case 3:                                 /* Average of left and above */
            if (first) {
                /* The row above is zero, so the average is half the left. */
                for (i = bpp; i < stride; i++) {
                    data[i] = (unsigned char)(data[i] + (data[i - bpp] >> 1));
                }
                break;
            }

            for (i = 0; i < bpp; i++) {
                data[i] = (unsigned char)(data[i] + (previous[i] >> 1));
            }

            for (; i < stride; i++) {
                data[i] = (unsigned char)(data[i]
                          + (((unsigned)data[i - bpp]
                              + (unsigned)previous[i]) / 2));
            }
            break;

        case 4:                                 /* Paeth */
            if (first) {
                /*
                 * With the row above zero, `paeth(left, 0, 0)` is `left` for
                 * every value - so the first line of a Paeth image is a Sub
                 * line. Written out rather than left to the general case,
                 * because the general case would be reading a row that is
                 * not there.
                 */
                for (i = bpp; i < stride; i++) {
                    data[i] = (unsigned char)(data[i] + data[i - bpp]);
                }
                break;
            }

            for (i = 0; i < bpp; i++) {
                data[i] = (unsigned char)(data[i]
                          + paeth(0, previous[i], 0));
            }

            if (bpp == 4) {
                for (; i + 4 <= stride; i += 4) {
                    i16x4 v = load4(data + i);

                    v += paeth4(load4(data + i - 4),
                                load4(previous + i),
                                load4(previous + i - 4));

                    store4(data + i, v);
                }
            }

            for (; i < stride; i++) {
                data[i] = (unsigned char)(data[i]
                          + paeth(data[i - bpp], previous[i],
                                  previous[i - bpp]));
            }
            break;

        default:
            return false;                       /* not a filter */
        }

        previous = data;
    }

    return true;
}

/*
 * The PNG, into whatever memory `room` gives.
 *
 * Every chunk, in order. Length, type, body, CRC - and the CRC is not
 * checked, deliberately: it guards against a corrupted file and this
 * decoder's bounds checks guard against a hostile one, which is the threat
 * that matters when the bytes came from somewhere else.
 */
int gfx_png_decode_into(const uint8_t *data, size_t length,
                        gfx_picture_room room, void *user,
                        unsigned *w_out, unsigned *h_out, const char **why_out)
{
    static const unsigned char signature[8] = {
        0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
    };

    unsigned long width = 0, height = 0;
    unsigned depth = 0, colour = 0, interlace = 0;
    unsigned channels = 0, bpp = 0;
    size_t stride = 0;

    unsigned char *idat = NULL;
    size_t idat_len = 0;

    unsigned char *rows = NULL;
    size_t rows_len = 0;

    /* The palette and its transparency, for colour type 3. On the stack
     * because the format bounds both: 256 entries, and no more. */
    unsigned char plte[256 * 3];
    unsigned char trns[256];
    unsigned plte_n = 0, trns_n = 0;

    struct gunzip_work *inflater = NULL;
    uint32_t *pixels;
    unsigned pitch = 0;
    size_t got = 0;
    size_t at;
    unsigned y, x;
    const char *why = NULL;
    int result = GFX_PICTURE_BAD;

    if (data == NULL || length < 8 || memcmp(data, signature, 8) != 0) {
        *why_out = "that is not a PNG: the signature is wrong";
        return GFX_PICTURE_NOT_MINE;
    }

    at = 8;

    while (at + 8 <= length) {
        uint32_t size = get_be32(data + at);
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

            width     = get_be32(body);
            height    = get_be32(body + 4);
            depth     = body[8];
            colour    = body[9];
            interlace = body[12];
        } else if (memcmp(type, "PLTE", 4) == 0) {
            /*
             * The palette, for colour type 3. Three bytes an entry and at
             * most 256 of them, which is the format's own limit rather
             * than one chosen here.
             */
            if (size > sizeof plte || (size % 3u) != 0) {
                why = "a palette that is not whole RGB triples";
                goto done;
            }

            memcpy(plte, body, size);
            plte_n = size / 3u;
        } else if (memcmp(type, "tRNS", 4) == 0) {
            /*
             * Transparency, one byte an entry, and entries the chunk does
             * not reach are opaque - which is what the format says and is
             * why this fills the rest rather than assuming 256 arrived.
             */
            if (size > sizeof trns) {
                why = "more transparency entries than palette entries";
                goto done;
            }

            memcpy(trns, body, size);
            trns_n = size;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            /*
             * The compressed data may be split across any number of chunks
             * and has to be inflated as one stream, so it is gathered first.
             * Sized once, from the file, because the total cannot exceed it.
             */
            if (idat == NULL) {
                idat = malloc(length);

                if (idat == NULL) {
                    why = "no room to gather the compressed data";
                    goto done;
                }
            }

            if (idat_len + size > length) {
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

    if (width > PICTURE_MAX || height > PICTURE_MAX) {
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
    case 3: channels = 1; break;                /* palette: one index */
    case 6: channels = 4; break;                /* RGBA */
    default:
        why = "a colour type this does not do - grey with alpha";
        goto done;
    }

    /*
     * **Palette, added because a plain colour could not be a wallpaper.**
     *
     * A 1920x1080 field of one colour is exactly what colour type 3 is
     * for, and every tool produces one that way - so `black.png`,
     * `blue.png` and `white.png` were all refused, and the window manager
     * said nothing about it because it threw the reason away. Diego, 21
     * September: "the appearance app does not rememver the wallpapers".
     * Both halves are fixed: this reads them, and `wm.lua` now says which
     * wallpaper it restored or why it could not.
     */
    if (colour == 3 && plte_n == 0) {
        why = "a palette image with no palette in it";
        goto done;
    }

    /* Its two bytes of zlib header, at the least, before any deflate. */
    if (idat == NULL || idat_len < 2) {
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
    rows_len = (stride + 1) * height;
    rows = malloc(rows_len);
    inflater = malloc(sizeof *inflater);

    if (rows == NULL || inflater == NULL) {
        why = "no room for the decoded rows";
        goto done;
    }

    /*
     * The zlib stream's two bytes of header off, and the deflate inflated
     * into exactly the room the header says the rows take: a stream that
     * wants more is refused there rather than written past the end.
     */
    switch (kosmos_inflate(idat + 2, idat_len - 2, inflater, rows, rows_len,
                           &got)) {
    case GUNZIP_WHOLE:
        break;
    case GUNZIP_REFUSED:
        why = "the image is a different size than its header says";
        goto done;
    default:
        why = "the compressed data would not inflate";
        goto done;
    }

    if (got != rows_len) {
        why = "the image is a different size than its header says";
        goto done;
    }

    if (!unfilter(rows, (unsigned)height, stride, bpp)) {
        why = "a row uses a filter that is not one of the five";
        goto done;
    }

    /* The file is good; now, and only now, the caller's memory. */
    pixels = room(user, (unsigned)width, (unsigned)height, &pitch);

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

            if (colour == 3) {
                unsigned idx = p[0];

                /*
                 * An index past the palette is a broken file, and black
                 * is the answer rather than a read past the end - the
                 * same choice the texture reader makes, and for the same
                 * reason: this data came from somewhere else.
                 */
                if (idx >= plte_n) {
                    r = g = b = 0;
                    a = 255;
                } else {
                    r = plte[idx * 3];
                    g = plte[idx * 3 + 1];
                    b = plte[idx * 3 + 2];
                    a = (idx < trns_n) ? trns[idx] : 255;
                }
            } else if (channels == 1) {
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

    *w_out = (unsigned)width;
    *h_out = (unsigned)height;
    result = GFX_PICTURE_OK;

done:
    free(inflater);
    free(rows);
    free(idat);

    if (result != GFX_PICTURE_OK) {
        *why_out = why ? why : "malformed";
    }

    return result;
}

/*
 * The JPEG, into whatever memory `room` gives.
 *
 * stb decodes into memory of its own, and the copy into the caller's is not
 * waste: stb's rows are RGBA in memory order at `width * 4`, and the
 * caller's are words at a pitch of its own, so there is a pass over every
 * pixel whatever happens.
 */
int gfx_jpeg_decode_into(const uint8_t *data, size_t length,
                         gfx_picture_room room, void *user,
                         unsigned *w_out, unsigned *h_out, const char **why_out)
{
    int width = 0, height = 0, channels = 0;
    unsigned char *rgba;
    uint32_t *pixels;
    unsigned pitch = 0;
    int y, x;

    if (data == NULL || length > (size_t)INT32_MAX) {
        *why_out = "larger than this system will decode";
        return GFX_PICTURE_BAD;
    }

    /*
     * Not a JPEG at all - no start-of-image marker - is said apart from a
     * JPEG that is broken, so a caller holding bytes of either kind can
     * ask one decoder and then the other. stb would refuse it too, with
     * its own sentence; this is the same answer, earlier and as a code.
     */
    if (length < 2 || data[0] != 0xff || data[1] != 0xd8) {
        *why_out = "not a JPEG";
        return GFX_PICTURE_NOT_MINE;
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

        *why_out = why ? why : "malformed";
        return GFX_PICTURE_BAD;
    }

    if (width <= 0 || height <= 0
        || (unsigned)width > PICTURE_MAX || (unsigned)height > PICTURE_MAX) {
        stbi_image_free(rgba);
        *why_out = "larger than this system will decode";
        return GFX_PICTURE_BAD;
    }

    pixels = room(user, (unsigned)width, (unsigned)height, &pitch);

    if (pixels == NULL) {
        stbi_image_free(rgba);
        *why_out = "no room for the picture";
        return GFX_PICTURE_BAD;
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

    *w_out = (unsigned)width;
    *h_out = (unsigned)height;
    return GFX_PICTURE_OK;
}

/*
 * The plain form's memory: rows of exactly the width, from `malloc`.
 * `user` is where the pointer goes, so the caller has it to give back.
 */
static uint32_t *tight_room(void *user, unsigned w, unsigned h, unsigned *pitch)
{
    uint32_t **kept = user;

    *kept = malloc((size_t)w * h * sizeof(uint32_t));
    *pitch = w * (unsigned)sizeof(uint32_t);
    return *kept;
}

int gfx_png_decode(const uint8_t *bytes, size_t n, uint32_t **pixels,
                   unsigned *w, unsigned *h, const char **why)
{
    *pixels = NULL;
    return gfx_png_decode_into(bytes, n, tight_room, pixels, w, h, why);
}

int gfx_jpeg_decode(const uint8_t *bytes, size_t n, uint32_t **pixels,
                    unsigned *w, unsigned *h, const char **why)
{
    *pixels = NULL;
    return gfx_jpeg_decode_into(bytes, n, tight_room, pixels, w, h, why);
}

void gfx_picture_free(uint32_t *pixels)
{
    free(pixels);
}
