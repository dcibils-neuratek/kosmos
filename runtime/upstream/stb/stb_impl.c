/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * stb_truetype and stb_image, instantiated once each.
 *
 * The header is the library: including it with STB_TRUETYPE_IMPLEMENTATION
 * defined emits the code, and that must happen in exactly one translation
 * unit. Doing it here rather than inside gfx.c keeps five thousand lines of
 * somebody else's code out of a file we edit, and keeps its compile time
 * off every edit of ours.
 *
 * Vendored under `runtime/upstream/` beside `puff`, unmodified, with its
 * licence in the file - which is `CLAUDE.md`'s rule for anything vendored.
 * It is dual-licensed public domain / MIT; the notice is at the bottom of
 * the header.
 *
 * **Its own warning, which is worth repeating where somebody will read it:**
 * "NO SECURITY GUARANTEE -- DO NOT USE THIS ON UNTRUSTED FONT FILES ... an
 * attacker can use it to read arbitrary memory." The fonts this system
 * loads are vendored in its own image, so that is not a live risk today.
 * The mitigation for the day it is: this runs at EL0 in whichever process
 * asked to draw text, so a malicious font kills that process and nothing
 * else. That is the microkernel earning its keep rather than an excuse to
 * be careless - a font from a disk should still be treated as hostile.
 */

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/*
 * And JPEG, for the same reason and with the same bargain.
 *
 * **Only JPEG.** `STBI_ONLY_JPEG` is what keeps this from becoming a second
 * PNG decoder sitting beside the one in `user/kits/gfx/picture_decode.c`,
 * and a second decoder for a format is worse than none: two answers to one
 * question, one of which is never exercised. PNG is ours, understood line by line, and it
 * is the format everything in the image is stored in. JPEG is somebody
 * else's, because a JPEG decoder is a discrete cosine transform and a
 * Huffman decoder and four chroma subsampling layouts, and writing that from
 * the specification would be a month spent learning something this project
 * is not about.
 *
 * The rest are off deliberately rather than by omission: no stdio, because
 * there is no `FILE` here and the bytes arrive as a region; no HDR and no
 * linear, because both want `pow` and `ldexp` for a format nothing reads.
 *
 * `STBI_ASSERT` is silenced. The library asserts on conditions it has
 * already checked, and an `assert` that cannot print and cannot be caught is
 * a process that dies without saying why - which is worse than the bounds
 * check that follows it. A malformed file comes back as a null pointer and a
 * sentence, which is what the caller can do something with.
 */
/*
 * **No thread locals**, which is not a preference but a link error.
 *
 * stb keeps `stbi__g_failure_reason` - the sentence behind
 * `stbi_failure_reason()` - in a `__thread` variable, and GCC implements
 * that on a bare-metal AArch64 target by calling `__emutls_get_address`,
 * which lives in a runtime this system does not link. `STBI_NO_THREAD_LOCALS`
 * is the library's own switch for exactly this.
 *
 * What it costs, stated rather than hidden: two threads in one process
 * decoding two malformed images at the same moment can read each other's
 * error message. That is a wrong sentence in a failure path, not a wrong
 * pixel - the decode itself keeps all of its state on the stack - and the
 * thing that opens pictures here is a window manager doing it one at a time.
 */
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ASSERT(x) ((void)0)
#include "stb_image.h"

/*
 * **And PNG out**, for a Cafesa3D render saved and the screen's picture
 * kept: stb_image_write, by the same author and under the same terms,
 * taken from the same commit. Only its PNG writer is used. No stdio, for
 * the reason there is none above; and its asserts are silenced, for the
 * reason STBI_ASSERT is.
 *
 * **Its deflate is miniz's**, the Compress Kit's deflater, rather than the
 * one stb carries - which was here because this image once had nothing
 * else that compressed (`cleanup.md` finding 106). On a 1920x1080 desktop
 * tdefl at level 6 made 331,980 bytes in 28 ms where stb's made 408,643 in
 * 40, and at 8 it took 64 ms for 2% less, so 6 it is, as zlib's default is.
 * Its state is about 320 KB, taken for the picture and given back.
 */
#include <stdlib.h>
#include <string.h>
#include "miniz.h"

struct png_zlib { unsigned char *bytes; size_t used, room; int failed; };

static mz_bool png_zlib_put(const void *from, int len, void *user)
{
    struct png_zlib *z = user;

    if (z->used + (size_t)len > z->room) {
        size_t room = (z->used + (size_t)len) * 2;
        unsigned char *grown = realloc(z->bytes, room);

        if (grown == NULL) {
            z->failed = 1;
            return MZ_FALSE;
        }

        z->bytes = grown, z->room = room;
    }

    memcpy(z->bytes + z->used, from, (size_t)len);
    z->used += (size_t)len;
    return MZ_TRUE;
}

static unsigned char *png_zlib(unsigned char *data, int len, int *out_len, int quality)
{
    tdefl_compressor *d = malloc(sizeof *d);
    struct png_zlib z = { NULL, 0, 0, 0 };
    mz_uint flags = tdefl_create_comp_flags_from_zip_params(6, MZ_DEFAULT_WINDOW_BITS,
                                                            MZ_DEFAULT_STRATEGY);
    tdefl_status st = TDEFL_STATUS_BAD_PARAM;

    (void)quality;

    if (d != NULL && tdefl_init(d, png_zlib_put, &z, (int)flags) == TDEFL_STATUS_OKAY) {
        st = tdefl_compress_buffer(d, data, (size_t)len, TDEFL_FINISH);
    }

    free(d);

    if (st != TDEFL_STATUS_DONE || z.failed) {
        free(z.bytes);
        return NULL;
    }

    *out_len = (int)z.used;
    return z.bytes;
}

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#define STBIW_ASSERT(x) ((void)0)
#define STBIW_ZLIB_COMPRESS png_zlib
#include "stb_image_write.h"
