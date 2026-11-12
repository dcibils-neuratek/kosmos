/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **W5a's pictures, held to this Mac** (`docs/w5.md`): zlib, libpng,
 * libjpeg-turbo and libwebp, each given the test pictures its own release
 * ships and asked what is in them. One line a picture, and every line is
 * what the same file says compiled natively on this Mac against the same
 * releases (`tools/run_pictures.py`) - so the guest is held byte for byte
 * to a reference it did not make.
 *
 * What is asked of each:
 *
 * - **zlib**: every file the manifest names, deflated at levels 1, 6 and 9
 *   and as gzip, each inflated back and compared; the compressed bytes'
 *   own fingerprint, since zlib is the same code everywhere and so is its
 *   output; its CRC-32 and Adler-32.
 * - **libpng**: each PNG through the simplified API to RGBA, and again
 *   through the progressive reader fed 512 bytes at a time, as WebKit
 *   feeds it a picture still arriving. PngSuite's broken files and the
 *   crashers must fail, and with the same sentence.
 * - **libjpeg-turbo**: each JPEG through libjpeg's API - the accurate
 *   integer DCT, the fast one, and at half size - 8 and 12 bits; and
 *   `testorig.ppm` encoded three ways.
 * - **libwebp**: each WebP decoded, `test.webp` held to `test_ref.ppm`,
 *   which is libwebp's own record of what it decodes to; and
 *   `testorig.ppm` encoded lossless and lossy and decoded back.
 *
 * A fingerprint is FNV-1a over 64 bits, ours rather than zlib's, so a
 * broken zlib cannot vouch for itself.
 *
 * This is ported code's test and links the POSIX layer: it reads its
 * pictures with `fopen` from the folder it is given.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>
#include <png.h>
#include <jpeglib.h>
#include <jerror.h>
#include <webp/decode.h>
#include <webp/encode.h>

#include "picturetest.h"

/* ---- what is said ---- */

static picturetest_say_fn say_fn;
static void *say_ctx;

static void say(const char *format, ...)
{
    char line[1024];
    va_list ap;

    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    say_fn(line, say_ctx);
}

static uint64_t fnv(const void *bytes, size_t length)
{
    const unsigned char *p = bytes;
    uint64_t h = 0xcbf29ce484222325ull;

    for (size_t i = 0; i < length; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static unsigned char *slurp(const char *path, size_t *length)
{
    FILE *f = fopen(path, "rb");
    unsigned char *bytes = NULL;
    size_t have = 0, room = 0;

    if (!f)
        return NULL;

    for (;;) {
        if (have == room) {
            room = room ? room * 2 : 65536;
            unsigned char *more = realloc(bytes, room);
            if (!more) {
                free(bytes);
                fclose(f);
                return NULL;
            }
            bytes = more;
        }
        size_t got = fread(bytes + have, 1, room - have, f);
        if (got == 0)
            break;
        have += got;
    }

    fclose(f);
    *length = have;
    return bytes;
}

/* A binary PPM (`P6`, 8 bits): libjpeg-turbo's `testorig.ppm` and
 * libwebp's `test_ref.ppm`. */
static unsigned char *ppm(const unsigned char *b, size_t n, int *w, int *h)
{
    size_t at = 2;
    int fields[3], got = 0;

    if (n < 2 || b[0] != 'P' || b[1] != '6')
        return NULL;

    while (got < 3 && at < n) {
        if (b[at] == '#') {
            while (at < n && b[at] != '\n')
                at++;
        } else if (b[at] >= '0' && b[at] <= '9') {
            int v = 0;
            while (at < n && b[at] >= '0' && b[at] <= '9')
                v = v * 10 + (b[at++] - '0');
            fields[got++] = v;
            continue;
        }
        at++;
    }

    if (got < 3 || fields[2] != 255)
        return NULL;
    at++;                                   /* the one whitespace after */
    *w = fields[0];
    *h = fields[1];
    if (at + (size_t)*w * *h * 3 > n)
        return NULL;
    return (unsigned char *)b + at;
}

/* ---- zlib ---- */

static void zlib_one(const char *name, const unsigned char *b, size_t n)
{
    char said[768];
    int at = snprintf(said, sizeof(said), "zlib %s: crc %08lx adler %08lx", name,
                      crc32(0, b, (uInt)n), adler32(1, b, (uInt)n));
    static const int levels[] = { 1, 6, 9 };
    uLong room = compressBound(n) + 64;
    unsigned char *packed = malloc(room), *back = malloc(n + 1);

    for (int i = 0; i < 3; i++) {
        uLongf length = room, out = n + 1;
        int r = compress2(packed, &length, b, n, levels[i]);
        if (r != Z_OK) {
            at += snprintf(said + at, sizeof(said) - at, " L%d error %d", levels[i], r);
            continue;
        }
        r = uncompress(back, &out, packed, length);
        at += snprintf(said + at, sizeof(said) - at, " L%d %lu %016llx%s", levels[i],
                       (unsigned long)length, (unsigned long long)fnv(packed, length),
                       r == Z_OK && out == n && memcmp(back, b, n) == 0 ? "" : " BACK-DIFFERS");
    }

    /* gzip, with the header's system byte said rather than taken from
     * whichever system compiled zlib: 19 on a Mac, 3 elsewhere. */
    z_stream z;
    gz_header header;
    memset(&z, 0, sizeof(z));
    memset(&header, 0, sizeof(header));
    header.os = 255;

    if (deflateInit2(&z, 6, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) == Z_OK
            && deflateSetHeader(&z, &header) == Z_OK) {
        z.next_in = (Bytef *)b;
        z.avail_in = n;
        z.next_out = packed;
        z.avail_out = room;
        int r = deflate(&z, Z_FINISH);
        uLong length = z.total_out;
        deflateEnd(&z);

        memset(&z, 0, sizeof(z));
        int ok = r == Z_STREAM_END && inflateInit2(&z, 47) == Z_OK;
        if (ok) {
            z.next_in = packed;
            z.avail_in = length;
            z.next_out = back;
            z.avail_out = n + 1;
            ok = inflate(&z, Z_FINISH) == Z_STREAM_END && z.total_out == n
                 && memcmp(back, b, n) == 0;
            inflateEnd(&z);
        }
        snprintf(said + at, sizeof(said) - at, " gzip %lu %016llx%s", (unsigned long)length,
                 (unsigned long long)fnv(packed, length), ok ? "" : " BACK-DIFFERS");
    }

    free(packed);
    free(back);
    say("%s", said);
}

/* ---- libpng ---- */

struct progressive {
    jmp_buf fail;
    char why[256];
    png_structp png;
    png_infop info;
    unsigned char *pixels;
    size_t rowbytes;
    png_uint_32 w, h;
    int done;
};

static void png_failed(png_structp png, png_const_charp message)
{
    struct progressive *p = png_get_error_ptr(png);
    snprintf(p->why, sizeof(p->why), "%s", message);
    longjmp(p->fail, 1);
}

static void png_warned(png_structp png, png_const_charp message)
{
    (void)png;
    (void)message;
}

/* What WebKit's `PNGImageDecoder` asks for: everything expanded to 8-bit
 * RGBA, interlacing undone by libpng. */
static void png_info_ready(png_structp png, png_infop info)
{
    struct progressive *p = png_get_progressive_ptr(png);
    int depth, colour;

    png_get_IHDR(png, info, &p->w, &p->h, &depth, &colour, NULL, NULL, NULL);
    png_set_expand(png);
    if (depth == 16)
        png_set_strip_16(png);
    if (colour == PNG_COLOR_TYPE_GRAY || colour == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    if (!(colour & PNG_COLOR_MASK_ALPHA) && !png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_filler(png, 0xff, PNG_FILLER_AFTER);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);

    p->rowbytes = png_get_rowbytes(png, info);
    if ((uint64_t)p->rowbytes * p->h > (64u << 20))
        png_error(png, "picturetest: too large to hold");
    p->pixels = calloc(p->h ? p->h : 1, p->rowbytes ? p->rowbytes : 1);
    if (!p->pixels)
        png_error(png, "picturetest: no memory");
}

static void png_row_ready(png_structp png, png_bytep row, png_uint_32 n, int pass)
{
    struct progressive *p = png_get_progressive_ptr(png);
    (void)pass;
    if (row && n < p->h)
        png_progressive_combine_row(png, p->pixels + n * p->rowbytes, row);
}

static void png_end(png_structp png, png_infop info)
{
    struct progressive *p = png_get_progressive_ptr(png);
    (void)info;
    p->done = 1;
}

static void png_one(const char *name, const unsigned char *b, size_t n)
{
    char simple[320], fed[320];

    /* The simplified API, to 8-bit RGBA. */
    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;

    if (png_image_begin_read_from_memory(&image, b, n)) {
        image.format = PNG_FORMAT_RGBA;
        size_t size = PNG_IMAGE_SIZE(image);
        unsigned char *pixels = size <= (64u << 20) ? malloc(size) : NULL;

        if (pixels && png_image_finish_read(&image, NULL, pixels, 0, NULL))
            snprintf(simple, sizeof(simple), "%ux%u %016llx", image.width, image.height,
                     (unsigned long long)fnv(pixels, size));
        else
            snprintf(simple, sizeof(simple), "error \"%s\"",
                     pixels ? image.message : "too large");
        free(pixels);
        png_image_free(&image);
    } else {
        snprintf(simple, sizeof(simple), "error \"%s\"", image.message);
    }

    /* The progressive reader, 512 bytes at a time. `volatile` for what
     * changes between the setjmp and a longjmp back to it. */
    struct progressive p;
    memset(&p, 0, sizeof(p));
    p.png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &p, png_failed, png_warned);
    p.info = p.png ? png_create_info_struct(p.png) : NULL;

    if (!p.info) {
        snprintf(fed, sizeof(fed), "error \"no reader\"");
    } else if (setjmp(p.fail)) {
        snprintf(fed, sizeof(fed), "error \"%s\"", p.why);
    } else {
        png_set_progressive_read_fn(p.png, &p, png_info_ready, png_row_ready, png_end);
        for (volatile size_t at = 0; at < n && !p.done; at += 512)
            png_process_data(p.png, p.info, (png_bytep)b + at, n - at < 512 ? n - at : 512);
        if (!p.done)
            snprintf(fed, sizeof(fed), "error \"ended early\"");
        else
            snprintf(fed, sizeof(fed), "%ux%u %016llx", p.w, p.h,
                     (unsigned long long)fnv(p.pixels, p.rowbytes * p.h));
    }

    if (p.png)
        png_destroy_read_struct(&p.png, p.info ? &p.info : NULL, NULL);
    free(p.pixels);
    say("png %s: simple %s; progressive %s", name, simple, fed);
}

static void png_encode(const char *name, const unsigned char *rgb, int w, int h)
{
    png_image image;
    png_alloc_size_t length = 0;

    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    image.width = w;
    image.height = h;
    image.format = PNG_FORMAT_RGB;

    if (!png_image_write_get_memory_size(image, length, 0, rgb, 0, NULL)) {
        say("png-encode %s: error \"%s\"", name, image.message);
        return;
    }
    unsigned char *out = malloc(length);
    if (!png_image_write_to_memory(&image, out, &length, 0, rgb, 0, NULL))
        say("png-encode %s: error \"%s\"", name, image.message);
    else
        say("png-encode %s: %lu %016llx", name, (unsigned long)length,
            (unsigned long long)fnv(out, length));
    free(out);
}

/* ---- libjpeg-turbo ---- */

struct jpeg_fail {
    struct jpeg_error_mgr mgr;
    jmp_buf back;
    char why[JMSG_LENGTH_MAX];
};

static void jpeg_failed(j_common_ptr c)
{
    struct jpeg_fail *f = (struct jpeg_fail *)c->err;
    f->mgr.format_message(c, f->why);
    longjmp(f->back, 1);
}

static void jpeg_quiet(j_common_ptr c, int level)
{
    (void)c;
    (void)level;
}

enum { JPEG_ACCURATE, JPEG_FAST, JPEG_HALF };

static void jpeg_decode(const unsigned char *b, size_t n, int how, char *said, size_t size)
{
    struct jpeg_decompress_struct c;
    struct jpeg_fail f;
    unsigned char *volatile pixels = NULL;

    memset(&c, 0, sizeof(c));
    c.err = jpeg_std_error(&f.mgr);
    f.mgr.error_exit = jpeg_failed;
    f.mgr.emit_message = jpeg_quiet;

    if (setjmp(f.back)) {
        snprintf(said, size, "error \"%s\"", f.why);
        jpeg_destroy_decompress(&c);
        free(pixels);
        return;
    }

    jpeg_create_decompress(&c);
    jpeg_mem_src(&c, b, n);
    jpeg_read_header(&c, TRUE);

    if (how == JPEG_FAST)
        c.dct_method = JDCT_IFAST;
    if (how == JPEG_HALF && c.data_precision == 8) {
        c.scale_num = 1;
        c.scale_denom = 2;
    }

    jpeg_start_decompress(&c);

    size_t sample = c.data_precision <= 8 ? 1 : 2;
    size_t row = (size_t)c.output_width * c.output_components * sample;
    pixels = malloc(row * c.output_height);

    while (c.output_scanline < c.output_height) {
        void *line = pixels + row * c.output_scanline;
        if (c.data_precision <= 8)
            jpeg_read_scanlines(&c, (JSAMPARRAY)&line, 1);
        else if (c.data_precision <= 12)
            jpeg12_read_scanlines(&c, (J12SAMPARRAY)&line, 1);
        else
            jpeg16_read_scanlines(&c, (J16SAMPARRAY)&line, 1);
    }

    snprintf(said, size, "%ux%ux%d p%d %016llx", c.output_width, c.output_height,
             c.output_components, c.data_precision,
             (unsigned long long)fnv(pixels, row * c.output_height));
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    free(pixels);
}

static void jpeg_one(const char *name, const unsigned char *b, size_t n)
{
    char accurate[256], fast[256], half[256];

    jpeg_decode(b, n, JPEG_ACCURATE, accurate, sizeof(accurate));
    jpeg_decode(b, n, JPEG_FAST, fast, sizeof(fast));
    jpeg_decode(b, n, JPEG_HALF, half, sizeof(half));
    say("jpeg %s: islow %s; ifast %s; half %s", name, accurate, fast, half);
}

enum { JPEG_420_75, JPEG_444_95, JPEG_PROGRESSIVE };

static void jpeg_encode(const char *name, const unsigned char *rgb, int w, int h, int how)
{
    static const char *names[] = { "q75 4:2:0", "q95 4:4:4", "q85 progressive" };
    struct jpeg_compress_struct c;
    struct jpeg_fail f;
    unsigned char *volatile out = NULL;
    unsigned long length = 0;

    memset(&c, 0, sizeof(c));
    c.err = jpeg_std_error(&f.mgr);
    f.mgr.error_exit = jpeg_failed;
    f.mgr.emit_message = jpeg_quiet;

    if (setjmp(f.back)) {
        say("jpeg-encode %s %s: error \"%s\"", name, names[how], f.why);
        jpeg_destroy_compress(&c);
        free(out);
        return;
    }

    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, (unsigned char **)&out, &length);
    c.image_width = w;
    c.image_height = h;
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    c.dct_method = JDCT_ISLOW;

    if (how == JPEG_420_75) {
        jpeg_set_quality(&c, 75, TRUE);
    } else if (how == JPEG_444_95) {
        jpeg_set_quality(&c, 95, TRUE);
        c.comp_info[0].h_samp_factor = c.comp_info[0].v_samp_factor = 1;
    } else {
        jpeg_set_quality(&c, 85, TRUE);
        jpeg_simple_progression(&c);
    }

    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) {
        JSAMPROW line = (JSAMPROW)rgb + (size_t)c.next_scanline * w * 3;
        jpeg_write_scanlines(&c, &line, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);

    say("jpeg-encode %s %s: %lu %016llx", name, names[how], length,
        (unsigned long long)fnv(out, length));
    free(out);
}

/* ---- libwebp ---- */

static void webp_one(const char *name, const unsigned char *b, size_t n)
{
    int w = 0, h = 0;
    uint8_t *pixels = WebPDecodeRGBA(b, n, &w, &h);

    if (!pixels) {
        say("webp %s: error \"not decoded\"", name);
        return;
    }
    say("webp %s: %dx%d %016llx", name, w, h,
        (unsigned long long)fnv(pixels, (size_t)w * h * 4));
    WebPFree(pixels);
}

/* `test.webp` against `test_ref.ppm`, libwebp's own `dwebp` check
 * (`doc/tools.md`). The PPM was written in 2010 and the decoder's
 * conversion from YUV has been made more exact since: 13,265 of its values
 * are one away from what libwebp 1.6.0 decodes, none further. So the check
 * is that every value is within one, and how many are not equal is said -
 * and held to this Mac exactly like everything else. */
static void webp_reference(const char *name, const unsigned char *b, size_t n,
                           const unsigned char *ref, size_t ref_n)
{
    int w = 0, h = 0, rw = 0, rh = 0;
    uint8_t *pixels = WebPDecodeRGB(b, n, &w, &h);
    const unsigned char *want = ppm(ref, ref_n, &rw, &rh);

    if (!pixels || !want || w != rw || h != rh) {
        say("webp-reference %s: DIFFERS (%dx%d against %dx%d)", name, w, h, rw, rh);
    } else {
        size_t length = (size_t)w * h * 3, off = 0, far = 0;
        for (size_t at = 0; at < length; at++) {
            int d = pixels[at] - want[at];
            off += d != 0;
            far += d > 1 || d < -1;
        }
        if (far == 0)
            say("webp-reference %s: within one of test_ref.ppm, %dx%d, %zu values one away",
                name, w, h, off);
        else
            say("webp-reference %s: DIFFERS from test_ref.ppm, %zu values more than one away",
                name, far);
    }
    WebPFree(pixels);
}

static void webp_encode(const char *name, const unsigned char *rgb, int w, int h)
{
    for (int lossless = 1; lossless >= 0; lossless--) {
        uint8_t *out = NULL;
        size_t length = lossless ? WebPEncodeLosslessRGB(rgb, w, h, w * 3, &out)
                                 : WebPEncodeRGB(rgb, w, h, w * 3, 75, &out);
        const char *how = lossless ? "lossless" : "q75";

        if (!length) {
            say("webp-encode %s %s: error \"not encoded\"", name, how);
            continue;
        }

        int dw = 0, dh = 0;
        uint8_t *back = WebPDecodeRGB(out, length, &dw, &dh);
        int same = back && dw == w && dh == h
                   && memcmp(back, rgb, (size_t)w * h * 3) == 0;
        say("webp-encode %s %s: %zu %016llx, decoded %016llx%s", name, how, length,
            (unsigned long long)fnv(out, length),
            (unsigned long long)(back ? fnv(back, (size_t)dw * dh * 3) : 0),
            lossless ? (same ? ", the same pixels" : ", NOT THE SAME PIXELS") : "");
        WebPFree(back);
        WebPFree(out);
    }
}

/* ---- the run ---- */

static int ends(const char *name, const char *with)
{
    size_t a = strlen(name), b = strlen(with);
    return a >= b && strcmp(name + a - b, with) == 0;
}

int picturetest_run(const char *folder, picturetest_say_fn fn, void *ctx)
{
    char path[512], name[256];
    size_t length;
    unsigned char *manifest;
    int count = 0;

    say_fn = fn;
    say_ctx = ctx;

    snprintf(path, sizeof(path), "%s/manifest.txt", folder);
    manifest = slurp(path, &length);
    if (!manifest) {
        say("picturetest: no %s", path);
        return -1;
    }

    say("versions: zlib %s, libpng %s, libjpeg-turbo %d, libwebp %06x", zlibVersion(),
        png_get_libpng_ver(NULL), LIBJPEG_TURBO_VERSION_NUMBER, WebPGetDecoderVersion());

    for (size_t at = 0; at < length;) {
        size_t end = at;
        while (end < length && manifest[end] != '\n')
            end++;
        if (end - at >= sizeof(name) || end == at) {
            at = end + 1;
            continue;
        }
        memcpy(name, manifest + at, end - at);
        name[end - at] = 0;
        at = end + 1;

        snprintf(path, sizeof(path), "%s/%s", folder, name);
        size_t n;
        unsigned char *b = slurp(path, &n);
        if (!b) {
            say("%s: not read", name);
            continue;
        }
        count++;

        zlib_one(name, b, n);

        if (ends(name, ".png")) {
            png_one(name, b, n);
        } else if (ends(name, ".jpg")) {
            jpeg_one(name, b, n);
        } else if (ends(name, ".webp")) {
            webp_one(name, b, n);
            if (ends(name, "/test.webp")) {
                char ref[512];
                size_t ref_n;
                snprintf(ref, sizeof(ref), "%s/%.*stest_ref.ppm", folder,
                         (int)(strlen(name) - strlen("test.webp")), name);
                unsigned char *r = slurp(ref, &ref_n);
                webp_reference(name, b, n, r, r ? ref_n : 0);
                free(r);
            }
        } else if (ends(name, "testorig.ppm")) {
            int w, h;
            const unsigned char *rgb = ppm(b, n, &w, &h);
            if (!rgb) {
                say("%s: not a PPM", name);
            } else {
                png_encode(name, rgb, w, h);
                for (int how = JPEG_420_75; how <= JPEG_PROGRESSIVE; how++)
                    jpeg_encode(name, rgb, w, h, how);
                webp_encode(name, rgb, w, h);
            }
        }
        free(b);
    }

    free(manifest);
    say("picturetest: %d files", count);
    return count;
}

#ifdef PICTURETEST_HOST
/* On this Mac, the reference: the folder the guest is given, the lines
 * on standard output. */
static void print_line(const char *line, void *ctx)
{
    (void)ctx;
    puts(line);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: picturetest FOLDER\n");
        return 2;
    }
    return picturetest_run(argv[1], print_line, NULL) < 0;
}
#endif
