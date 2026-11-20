/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * gfx's PNG and JPEG decoders, called as plain C with no Lua state
 * (`user/kits/gfx/picture_decode.c`) - the way Flight's terrain builder
 * calls them from a thread of its own.
 *
 * The pictures and what each must decode to come from
 * `tools/picture_fixtures.py`, run first into a directory this is handed:
 *
 *   - every PNG to the word, against pixels known before the file was
 *     written or worked out by the Python's own decoder: each colour type,
 *     each filter alone and in turn, widths either side of the vector
 *     paths, split image data, a terrarium tile, and the PNGs in `assets/`;
 *   - every JPEG to the word against stb_image called directly and its
 *     bytes turned into words here - what `gfx.jpeg` did before the
 *     decoder left it - and close to macOS's own decoder, which is a
 *     second opinion with a different IDCT;
 *   - each refusal with its code and its words, and no memory asked for;
 *   - both again into rows with a pitch wider than the picture, as a
 *     surface's is, with the padding left as it was;
 *   - and eight threads decoding at once, each answer the same as one
 *     thread's, because a decoder that kept anything between calls would
 *     tear there and nowhere else.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "picture_decode.h"
#include "stb_image.h"

static int failures, checks;
static const char *dir;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            printf("FAIL: ");                                               \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

static double now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* A file whole: relative to the fixtures' directory, unless absolute. */
static uint8_t *slurp(const char *name, size_t *n)
{
    char path[1024];
    FILE *f;
    uint8_t *bytes;
    long size;

    if (name[0] == '/') {
        snprintf(path, sizeof path, "%s", name);
    } else {
        snprintf(path, sizeof path, "%s/%s", dir, name);
    }

    f = fopen(path, "rb");
    if (f == NULL) {
        printf("FAIL: cannot open %s\n", path);
        exit(1);
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    bytes = malloc((size_t)size + 1);
    if (bytes == NULL || fread(bytes, 1, (size_t)size, f) != (size_t)size) {
        printf("FAIL: cannot read %s\n", path);
        exit(1);
    }
    fclose(f);
    *n = (size_t)size;
    return bytes;
}

/* An `.argb`: the words the decode must give, little-endian on disk. */
static uint32_t *expected(const char *name, size_t *count)
{
    size_t n, i;
    uint8_t *b = slurp(name, &n);
    uint32_t *w = malloc(n);

    for (i = 0; i < n / 4; i++) {
        w[i] = (uint32_t)b[i * 4] | (uint32_t)b[i * 4 + 1] << 8
             | (uint32_t)b[i * 4 + 2] << 16 | (uint32_t)b[i * 4 + 3] << 24;
    }
    free(b);
    *count = n / 4;
    return w;
}

/*
 * Rows with a pitch wider than the picture, the rest of each row a value
 * the decoder has no reason to write - which is what a surface's 64-byte
 * pitch looks like to it. `asked` counts the calls, so a refusal can be
 * shown to have asked for nothing.
 */
#define PADDING 0xdeadbeefu

struct padded {
    uint32_t *words;
    unsigned  pitch;        /* bytes */
    size_t    total;        /* words */
    int       asked;
    int       refuse;       /* answer NULL, as a full machine would */
};

static uint32_t *padded_room(void *user, unsigned w, unsigned h,
                             unsigned *pitch)
{
    struct padded *p = user;
    size_t i;

    p->asked++;
    if (p->refuse) {
        return NULL;
    }

    p->pitch = ((w * 4u + 63u) & ~63u) + 64u;
    p->total = (size_t)p->pitch / 4u * h;
    p->words = malloc(p->total * 4u);
    for (i = 0; i < p->total; i++) {
        p->words[i] = PADDING;
    }
    *pitch = p->pitch;
    return p->words;
}

/* The padded decode holds the same pixels as the tight one, and nothing
 * past each row's width was written. */
static void check_padded(const char *what, const struct padded *p,
                         const uint32_t *tight, unsigned w, unsigned h)
{
    unsigned x, y, wrong = 0, stray = 0;
    unsigned stride = p->pitch / 4u;

    for (y = 0; y < h; y++) {
        for (x = 0; x < stride; x++) {
            uint32_t v = p->words[(size_t)y * stride + x];

            if (x < w && v != tight[(size_t)y * w + x]) {
                wrong++;
            } else if (x >= w && v != PADDING) {
                stray++;
            }
        }
    }

    CHECK(wrong == 0 && stray == 0,
          "%s at a pitch of %u: %u pixels differ, %u padding words written",
          what, p->pitch, wrong, stray);
}

static void check_png(const char *file, const char *argb)
{
    size_t n, count, i, differ = 0;
    uint8_t *bytes = slurp(file, &n);
    uint32_t *want = expected(argb, &count);
    uint32_t *pixels = NULL;
    unsigned w = 0, h = 0;
    const char *why = "";
    int r = gfx_png_decode(bytes, n, &pixels, &w, &h, &why);
    struct padded p = { 0 };
    unsigned pw = 0, ph = 0;

    CHECK(r == GFX_PICTURE_OK, "%s refused: %s", file, why);
    if (r != GFX_PICTURE_OK) {
        goto out;
    }

    CHECK((size_t)w * h == count, "%s is %ux%u, the reference has %zu pixels",
          file, w, h, count);
    if ((size_t)w * h == count) {
        for (i = 0; i < count; i++) {
            if (pixels[i] != want[i]) {
                if (differ == 0) {
                    printf("  %s: first difference at (%zu, %zu): "
                           "%08x, wanted %08x\n", file, i % w, i / w,
                           pixels[i], want[i]);
                }
                differ++;
            }
        }
        CHECK(differ == 0, "%s: %zu of %zu pixels differ", file, differ,
              count);
    }

    r = gfx_png_decode_into(bytes, n, padded_room, &p, &pw, &ph, &why);
    CHECK(r == GFX_PICTURE_OK && pw == w && ph == h && p.asked == 1,
          "%s into padded rows: %d, %ux%u, asked %d times", file, r, pw, ph,
          p.asked);
    if (r == GFX_PICTURE_OK) {
        check_padded(file, &p, pixels, w, h);
    }

out:
    free(p.words);
    gfx_picture_free(pixels);
    free(want);
    free(bytes);
}

/*
 * What `gfx.jpeg` did before the decoder moved, written out again here as
 * the reference: stb's RGBA bytes, a component at a time, into words.
 */
static uint32_t *stb_reference(const uint8_t *bytes, size_t n, int *w, int *h)
{
    int c, i;
    unsigned char *rgba = stbi_load_from_memory(bytes, (int)n, w, h, &c, 4);
    uint32_t *words;

    if (rgba == NULL) {
        return NULL;
    }

    words = malloc((size_t)*w * (size_t)*h * 4u);
    for (i = 0; i < *w * *h; i++) {
        words[i] = (uint32_t)rgba[i * 4 + 3] << 24
                 | (uint32_t)rgba[i * 4] << 16
                 | (uint32_t)rgba[i * 4 + 1] << 8 | rgba[i * 4 + 2];
    }
    stbi_image_free(rgba);
    return words;
}

static void check_jpeg(const char *file, const char *argb, unsigned rw,
                       unsigned rh)
{
    size_t n, count, i, differ = 0;
    uint8_t *bytes = slurp(file, &n);
    uint32_t *apple = expected(argb, &count);
    uint32_t *pixels = NULL, *stb;
    unsigned w = 0, h = 0, pw = 0, ph = 0;
    const char *why = "";
    int sw = 0, sh = 0;
    int r = gfx_jpeg_decode(bytes, n, &pixels, &w, &h, &why);
    struct padded p = { 0 };
    double total = 0;
    unsigned worst = 0, translucent = 0;

    CHECK(r == GFX_PICTURE_OK, "%s refused: %s", file, why);
    if (r != GFX_PICTURE_OK) {
        goto out;
    }

    CHECK(w == rw && h == rh, "%s is %ux%u, macOS says %ux%u", file, w, h,
          rw, rh);

    /* To the word, against stb as `gfx.jpeg` used to call it. */
    stb = stb_reference(bytes, n, &sw, &sh);
    CHECK(stb != NULL && (unsigned)sw == w && (unsigned)sh == h,
          "%s: stb alone did not give the same size", file);
    if (stb != NULL && (unsigned)sw == w && (unsigned)sh == h) {
        for (i = 0; i < (size_t)w * h; i++) {
            differ += pixels[i] != stb[i];
        }
        CHECK(differ == 0, "%s: %zu pixels differ from stb's own", file,
              differ);
    }
    free(stb);

    /*
     * Close to macOS's decoder. Two correct decoders differ by rounding in
     * the IDCT and by how chroma is upsampled, which moves an edge by tens
     * and a flat field by a unit or two - so the mean is what is held, and
     * 3 is inside "the same picture" and nowhere near a channel swapped or
     * a row misplaced, which is a mean of tens.
     */
    if (w == rw && h == rh && count == (size_t)w * h) {
        for (i = 0; i < count; i++) {
            int s;

            for (s = 0; s < 24; s += 8) {
                int a = (int)(pixels[i] >> s & 255), b = (int)(apple[i] >> s & 255);
                unsigned d = (unsigned)(a > b ? a - b : b - a);

                total += d;
                worst = d > worst ? d : worst;
            }
            if ((pixels[i] >> 24) != 255) {
                translucent++;
            }
        }
        total /= (double)count * 3.0;
        printf("  %s: %ux%u, mean %.2f and worst %u from macOS's decoder\n",
               file, w, h, total, worst);
        CHECK(total <= 3.0, "%s: a mean of %.2f from macOS's decoder", file,
              total);
        CHECK(translucent == 0, "%s: %u pixels of a JPEG not opaque", file,
              translucent);
    }

    r = gfx_jpeg_decode_into(bytes, n, padded_room, &p, &pw, &ph, &why);
    CHECK(r == GFX_PICTURE_OK && pw == w && ph == h && p.asked == 1,
          "%s into padded rows: %d, %ux%u", file, r, pw, ph);
    if (r == GFX_PICTURE_OK) {
        check_padded(file, &p, pixels, w, h);
    }

out:
    free(p.words);
    gfx_picture_free(pixels);
    free(apple);
    free(bytes);
}

static void check_refusal(const char *format, const char *file, int code,
                          const char *words)
{
    size_t n;
    uint8_t *bytes = slurp(file, &n);
    uint32_t *pixels = (uint32_t *)1;
    unsigned w, h;
    const char *why = NULL;
    struct padded p = { 0 };
    int r, jpeg = strcmp(format, "jpeg") == 0;

    r = jpeg ? gfx_jpeg_decode(bytes, n, &pixels, &w, &h, &why)
             : gfx_png_decode(bytes, n, &pixels, &w, &h, &why);
    CHECK(r == code && pixels == NULL && why != NULL
          && (strcmp(words, "-") == 0 || strstr(why, words) != NULL),
          "%s %s: %d \"%s\", wanted %d \"%s\"", format, file, r,
          why ? why : "(none)", code, words);

    r = jpeg ? gfx_jpeg_decode_into(bytes, n, padded_room, &p, &w, &h, &why)
             : gfx_png_decode_into(bytes, n, padded_room, &p, &w, &h, &why);
    CHECK(r == code && p.asked == 0,
          "%s %s refused, yet asked for room %d times", format, file,
          p.asked);
    free(bytes);
}

/* A machine with no room left: the decode says so and gives nothing. */
static void check_no_room(const char *file, int jpeg)
{
    size_t n;
    uint8_t *bytes = slurp(file, &n);
    struct padded p = { 0 };
    unsigned w, h;
    const char *why = NULL;
    int r;

    p.refuse = 1;
    r = jpeg ? gfx_jpeg_decode_into(bytes, n, padded_room, &p, &w, &h, &why)
             : gfx_png_decode_into(bytes, n, padded_room, &p, &w, &h, &why);
    CHECK(r == GFX_PICTURE_BAD && p.asked == 1 && why != NULL
          && strstr(why, "no room for the picture") != NULL,
          "%s with no room: %d \"%s\"", file, r, why ? why : "(none)");
    free(bytes);
}

/* Eight threads, each decoding the same two pictures again and again. */
struct worker {
    const uint8_t *png, *jpeg;
    size_t png_n, jpeg_n;
    const uint32_t *png_want, *jpeg_want;
    size_t png_count, jpeg_count;
    int wrong;
};

static void *work(void *arg)
{
    struct worker *k = arg;
    int round;

    for (round = 0; round < 12; round++) {
        uint32_t *px;
        unsigned w, h;
        const char *why;

        if (gfx_png_decode(k->png, k->png_n, &px, &w, &h, &why) != 0
            || (size_t)w * h != k->png_count
            || memcmp(px, k->png_want, k->png_count * 4) != 0) {
            k->wrong++;
        }
        gfx_picture_free(px);

        if (gfx_jpeg_decode(k->jpeg, k->jpeg_n, &px, &w, &h, &why) != 0
            || (size_t)w * h != k->jpeg_count
            || memcmp(px, k->jpeg_want, k->jpeg_count * 4) != 0) {
            k->wrong++;
        }
        gfx_picture_free(px);
    }
    return NULL;
}

static void check_threads(const char *jpeg)
{
    enum { THREADS = 8 };
    pthread_t t[THREADS];
    struct worker k[THREADS], one = { 0 };
    uint32_t *png_want, *jpeg_want;
    unsigned w, h;
    const char *why;
    int i, wrong = 0;

    one.png = slurp("terrarium.png", &one.png_n);
    one.jpeg = slurp(jpeg, &one.jpeg_n);

    gfx_png_decode(one.png, one.png_n, &png_want, &w, &h, &why);
    one.png_count = (size_t)w * h;
    gfx_jpeg_decode(one.jpeg, one.jpeg_n, &jpeg_want, &w, &h, &why);
    one.jpeg_count = (size_t)w * h;
    one.png_want = png_want;
    one.jpeg_want = jpeg_want;

    for (i = 0; i < THREADS; i++) {
        k[i] = one;
        pthread_create(&t[i], NULL, work, &k[i]);
    }
    for (i = 0; i < THREADS; i++) {
        pthread_join(t[i], NULL);
        wrong += k[i].wrong;
    }

    CHECK(wrong == 0, "%d of %d decodes on %d threads at once differed "
          "from one thread's", wrong, THREADS * 24, THREADS);
    gfx_picture_free(png_want);
    gfx_picture_free(jpeg_want);
    free((void *)one.png);
    free((void *)one.jpeg);
}

/* How long a wallpaper-sized picture takes, printed rather than held. */
static void time_one(const char *file, int jpeg)
{
    size_t n;
    uint8_t *bytes = slurp(file, &n);
    uint32_t *px;
    unsigned w = 0, h = 0;
    const char *why;
    double t0 = now();
    int i;

    for (i = 0; i < 5; i++) {
        if (jpeg) {
            gfx_jpeg_decode(bytes, n, &px, &w, &h, &why);
        } else {
            gfx_png_decode(bytes, n, &px, &w, &h, &why);
        }
        gfx_picture_free(px);
    }
    printf("  %s, %ux%u: %.2f ms a decode\n", file, w, h,
           (now() - t0) / 5 * 1000.0);
    free(bytes);
}

int main(int argc, char **argv)
{
    char path[1024], line[2048];
    FILE *manifest;
    char *last_png = NULL, *last_jpeg = NULL;

    if (argc != 2) {
        fprintf(stderr, "usage: %s FIXTURES (from tools/picture_fixtures.py)\n",
                argv[0]);
        return 2;
    }
    dir = argv[1];

    snprintf(path, sizeof path, "%s/manifest.txt", dir);
    manifest = fopen(path, "r");
    if (manifest == NULL) {
        printf("FAIL: no %s - run tools/picture_fixtures.py first\n", path);
        return 1;
    }

    while (fgets(line, sizeof line, manifest) != NULL) {
        char kind[16], a[1024], b[1024], words[512];
        unsigned rw, rh;
        int code;

        line[strcspn(line, "\n")] = 0;
        if (sscanf(line, "png %1023s %1023s", a, b) == 2) {
            check_png(a, b);
            if (strstr(a, "image00") != NULL) {
                free(last_png);
                last_png = strdup(a);
            }
        } else if (sscanf(line, "jpeg %1023s %1023s %u %u", a, b, &rw,
                          &rh) == 4) {
            check_jpeg(a, b, rw, rh);
            free(last_jpeg);
            last_jpeg = strdup(a);
        } else if (sscanf(line, "refuse %15s %1023s %d %511[^\n]", kind, a,
                          &code, words) == 4) {
            check_refusal(kind, a, code, words);
        } else if (line[0] != 0) {
            printf("FAIL: the manifest says \"%s\"\n", line);
            failures++;
        }
    }
    fclose(manifest);

    check_no_room("terrarium.png", 0);
    if (last_jpeg != NULL) {
        check_no_room(last_jpeg, 1);
        check_threads(last_jpeg);
        time_one(last_jpeg, 1);
    }
    if (last_png != NULL) {
        time_one(last_png, 0);
    }
    free(last_png);
    free(last_jpeg);

    printf("%s: %d of %d checks passed\n", failures ? "FAIL" : "ok",
           checks - failures, checks);
    return failures ? 1 : 0;
}
