/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The blur (`user/kits/gfx/blur.c`, Liquid Glass LG2) held to itself and to
 * arithmetic.
 *
 *   1. Eight at a time is a pixel at a time, to the bit: random pictures of
 *      awkward sizes - narrower than eight, a tail of seven, a radius past
 *      the picture - at several radii and passes, into a separate picture
 *      and in place, with a pitch wider than the picture.
 *   2. A pass is the true average, rounded, within one: the scalar loop
 *      against a box sum taken in doubles, channel by channel.
 *   3. A plain colour comes back exactly, at every radius to the most.
 *   4. Nothing outside the picture is written: the pitch's spare columns
 *      keep what they held.
 *   5. A radius of 0, or past the most, is a copy.
 *   6. The screen's size timed both ways: one wallpaper's glass, each
 *      way the best of three.
 *   7. The nebula (Starlight S3) eight at a time is a pixel at a time, to
 *      the bit, at awkward sizes and grains, past the pitch untouched.
 *   8. Without grain, a plain gradient is exact arithmetic: its first row
 *      is `top` over the glass and its last `bottom`, divided by 255
 *      exactly, and alpha the pixel's own.
 *   9. The grain moves a pixel by at most its amount, and the same on its
 *      three channels away from the ends of a byte; and it is dust - more
 *      than one value in a row of a plain colour, and the same every time.
 *  10. A grain past the most is the most.
 *  11. A screen's nebula timed: once a wallpaper.
 *
 * Built natively (NEON) and as x86-64 under Rosetta (SSE2), as `test_pack`
 * is, so both vector units are held to the scalar loop.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "blur.h"

static unsigned failures, checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("  %s\n", what);
    }
}

static uint32_t seed = 12345u;

static uint32_t next(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static unsigned clampu(long i, unsigned n)
{
    return i < 0 ? 0u : (unsigned long)i >= n ? n - 1u : (unsigned)i;
}

/* 1 and 4: vector and scalar, the same pixels and the same spare columns. */
static void same(unsigned w, unsigned h, unsigned r, unsigned passes,
                 int in_place)
{
    size_t pitch = w + 5u;
    size_t n = pitch * h;
    uint32_t *src = malloc(n * 4), *a = malloc(n * 4), *b = malloc(n * 4);
    uint32_t *tmp = malloc((size_t)w * h * 4), *sums = malloc((size_t)w * 16);
    char what[160];
    size_t i;
    unsigned x, y, bad = 0, spare = 0;

    for (i = 0; i < n; i++) src[i] = next();
    for (i = 0; i < n; i++) a[i] = b[i] = 0xDEADBEEFu;

    if (in_place) {
        memcpy(a, src, n * 4);
        memcpy(b, src, n * 4);
        gfx_blur(a, a, pitch, w, h, r, passes, tmp, sums);
        gfx_blur_scalar(b, b, pitch, w, h, r, passes, tmp, sums);
    } else {
        gfx_blur(src, a, pitch, w, h, r, passes, tmp, sums);
        gfx_blur_scalar(src, b, pitch, w, h, r, passes, tmp, sums);
    }

    for (y = 0; y < h; y++) {
        for (x = 0; x < pitch; x++) {
            size_t at = (size_t)y * pitch + x;

            if (x < w) {
                if (a[at] != b[at]) bad++;
            } else {
                uint32_t want = in_place ? src[at] : 0xDEADBEEFu;

                if (a[at] != want || b[at] != want) spare++;
            }
        }
    }

    snprintf(what, sizeof(what), "%ux%u r %u, %u passes%s: %u pixels differ "
             "eight at a time from one at a time", w, h, r, passes,
             in_place ? ", in place" : "", bad);
    check(bad == 0, what);
    snprintf(what, sizeof(what), "%ux%u r %u: %u pixels outside the picture "
             "were written", w, h, r, spare);
    check(spare == 0, what);

    free(src); free(a); free(b); free(tmp); free(sums);
}

/* 2: one pass against the true average. */
static void true_mean(unsigned w, unsigned h, unsigned r)
{
    size_t n = (size_t)w * h;
    uint32_t *src = malloc(n * 4), *out = malloc(n * 4), *tmp = malloc(n * 4);
    uint32_t *sums = malloc((size_t)w * 16);
    double *across = malloc(n * 4 * sizeof(double));
    unsigned x, y, c, worst = 0;
    long k;
    char what[160];
    size_t i;

    for (i = 0; i < n; i++) src[i] = next();

    gfx_blur_scalar(src, out, w, w, h, r, 1, tmp, sums);

    /* The pass in doubles: across, unrounded, then down. The vector unit's
     * answer rounds between the two, so within one of this, not equal. */
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            for (c = 0; c < 4; c++) {
                double s = 0;

                for (k = -(long)r; k <= (long)r; k++) {
                    s += (src[(size_t)y * w + clampu((long)x + k, w)] >> (8 * c)) & 0xFF;
                }
                across[((size_t)y * w + x) * 4 + c] = s / (2 * r + 1);
            }
        }
    }

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            for (c = 0; c < 4; c++) {
                double s = 0;
                long got, want;
                unsigned d;

                for (k = -(long)r; k <= (long)r; k++) {
                    s += across[((size_t)clampu((long)y + k, h) * w + x) * 4 + c];
                }
                want = lround(s / (2 * r + 1));
                got = (out[(size_t)y * w + x] >> (8 * c)) & 0xFF;
                d = (unsigned)labs(got - want);
                if (d > worst) worst = d;
            }
        }
    }

    snprintf(what, sizeof(what), "%ux%u r %u: a channel %u from the true "
             "average", w, h, r, worst);
    check(worst <= 1, what);

    free(src); free(out); free(tmp); free(sums); free(across);
}

/* 3: a plain colour, at every radius. */
static void plain(void)
{
    enum { W = 37, H = 21 };
    static uint32_t pic[W * H], tmp[W * H], sums[4 * W];
    unsigned r, bad = 0, i;
    char what[120];

    for (r = 1; r <= GFX_BLUR_MOST; r++) {
        uint32_t c = next() | 0xFF000000u;

        if (r % 3 == 0) c = 0xFFFFFFFFu;
        for (i = 0; i < W * H; i++) pic[i] = c;
        gfx_blur(pic, pic, W, W, H, r, 3, tmp, sums);
        for (i = 0; i < W * H; i++) {
            if (pic[i] != c) { bad++; break; }
        }
    }

    snprintf(what, sizeof(what), "%u radii of %u gave a plain colour back "
             "changed", bad, GFX_BLUR_MOST);
    check(bad == 0, what);
}

/* 5: nothing to blur by. */
static void copies(void)
{
    enum { W = 9, H = 4 };
    uint32_t src[W * H], dst[W * H], tmp[W * H], sums[4 * W];
    unsigned i, r[2] = { 0, GFX_BLUR_MOST + 1 }, j;

    for (j = 0; j < 2; j++) {
        for (i = 0; i < W * H; i++) { src[i] = next(); dst[i] = 0; }
        gfx_blur(src, dst, W, W, H, r[j], 3, tmp, sums);
        check(memcmp(src, dst, sizeof(src)) == 0,
              j ? "a radius past the most was not a copy"
                : "a radius of 0 was not a copy");
    }
}

/* 7: the nebula's vector loop held to its scalar one. */
static void nebula_same(unsigned w, unsigned h, unsigned grain)
{
    size_t pitch = w + 3u, n = pitch * h, i;
    uint32_t *a = malloc(n * 4), *b = malloc(n * 4);
    uint32_t top = next(), bottom = next(), s = next();
    char what[120];

    for (i = 0; i < n; i++) a[i] = b[i] = next();
    gfx_nebula(a, pitch, w, h, top, bottom, grain, s);
    gfx_nebula_scalar(b, pitch, w, h, top, bottom, grain, s);
    snprintf(what, sizeof(what), "the nebula at %ux%u, grain %u, is not the "
             "scalar loop's", w, h, grain);
    check(memcmp(a, b, n * 4) == 0, what);
    free(a);
    free(b);
}

static unsigned over255(unsigned c, unsigned a, unsigned v)
{
    return (c * a + v * (255u - a) + 127u) / 255u;
}

/* 8 to 10. */
static void nebula_plain(void)
{
    enum { W = 37, H = 21 };
    static uint32_t p[W * H], q[W * H];
    const uint32_t glass = 0x80203040u, top = 0x66a0b0ffu, bottom = 0xcc100820u;
    unsigned x, k, wrong = 0, far = 0, apart = 0, kinds = 0;
    uint32_t first;

    for (x = 0; x < W * H; x++) p[x] = glass;
    gfx_nebula(p, W, W, H, top, bottom, 0, 7u);

    for (x = 0; x < W; x++) {
        for (k = 0; k < 24; k += 8) {
            unsigned v = (glass >> k) & 0xFFu;

            if (((p[x] >> k) & 0xFFu) != over255((top >> k) & 0xFFu, top >> 24, v)) wrong++;
            if (((p[(H - 1) * W + x] >> k) & 0xFFu)
                != over255((bottom >> k) & 0xFFu, bottom >> 24, v)) wrong++;
        }
        if ((p[x] >> 24) != 0x80u || (p[(H - 1) * W + x] >> 24) != 0x80u) wrong++;
    }
    check(wrong == 0, "a plain gradient's first and last rows are not top and "
                      "bottom over the glass, exactly");

    /* 9: a mid grey, so no channel meets the end of a byte. */
    for (x = 0; x < W * H; x++) p[x] = q[x] = 0xff808080u;
    gfx_nebula(p, W, W, H, 0x40000000u, 0x40000000u, 0, 9u);
    gfx_nebula(q, W, W, H, 0x40000000u, 0x40000000u, 6, 9u);
    first = q[0];
    for (x = 0; x < W * H; x++) {
        int d = (int)(q[x] & 0xFFu) - (int)(p[x] & 0xFFu);

        if (d < -6 || d > 6) far++;
        if (((q[x] >> 8) & 0xFFu) != (q[x] & 0xFFu)
            || ((q[x] >> 16) & 0xFFu) != (q[x] & 0xFFu)) apart++;
        if (x < W && q[x] != first) kinds++;
    }
    check(far == 0, "the grain moved a pixel further than its amount");
    check(apart == 0, "the grain was not the same on a pixel's three channels");
    check(kinds > 0, "a row of a plain colour had one value: no grain");

    for (x = 0; x < W * H; x++) p[x] = 0xff808080u;
    gfx_nebula(p, W, W, H, 0x40000000u, 0x40000000u, 6, 9u);
    check(memcmp(p, q, sizeof(p)) == 0, "the same seed gave different dust");

    /* 10. */
    for (x = 0; x < W * H; x++) p[x] = q[x] = 0xff808080u;
    gfx_nebula(p, W, W, H, 0, 0, GFX_NEBULA_GRAIN_MOST + 40u, 3u);
    gfx_nebula(q, W, W, H, 0, 0, GFX_NEBULA_GRAIN_MOST, 3u);
    check(memcmp(p, q, sizeof(p)) == 0, "a grain past the most was not the most");
}

static double now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(void)
{
    static const unsigned sizes[][2] = {
        { 1, 1 }, { 3, 2 }, { 7, 9 }, { 8, 8 }, { 15, 17 }, { 64, 33 },
        { 100, 70 }, { 257, 31 },
    };
    static const unsigned radii[] = { 1, 2, 5, 8, 40 };
    unsigned i, j, k;
    double t0, vec, sca, nvec, nsca;
    uint32_t *pic, *tmp, *sums;
    const unsigned W = 1920, H = 1080, R = 8;

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        for (j = 0; j < sizeof(radii) / sizeof(radii[0]); j++) {
            for (k = 1; k <= 3; k += 2) {
                same(sizes[i][0], sizes[i][1], radii[j], k, 0);
            }
            same(sizes[i][0], sizes[i][1], radii[j], 3, 1);
        }
    }

    true_mean(23, 19, 1);
    true_mean(23, 19, 4);
    true_mean(40, 9, 30);
    plain();
    copies();

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        nebula_same(sizes[i][0], sizes[i][1], 0);
        nebula_same(sizes[i][0], sizes[i][1], 5);
        nebula_same(sizes[i][0], sizes[i][1], GFX_NEBULA_GRAIN_MOST);
    }
    nebula_plain();

    pic = malloc((size_t)W * H * 4);
    tmp = malloc((size_t)W * H * 4);
    sums = malloc((size_t)W * 16);
    for (i = 0; i < W * H; i++) pic[i] = next();

    /* Each the best of three, after one that touches every page: the first
     * to run paid for faulting `tmp` in, which is not the blur's cost. */
    gfx_blur(pic, pic, W, W, H, R, 1, tmp, sums);
    vec = sca = 1e9;
    for (i = 0; i < 3; i++) {
        double d;

        t0 = now_ms();
        gfx_blur(pic, pic, W, W, H, R, 3, tmp, sums);
        d = now_ms() - t0;
        if (d < vec) vec = d;
        t0 = now_ms();
        gfx_blur_scalar(pic, pic, W, W, H, R, 3, tmp, sums);
        d = now_ms() - t0;
        if (d < sca) sca = d;
    }

    /* 11: the nebula over the same screen, the best of three. */
    nvec = nsca = 1e9;
    for (i = 0; i < 3; i++) {
        double d;

        t0 = now_ms();
        gfx_nebula(pic, W, W, H, 0x40303060u, 0x60101030u, 6, 1u);
        d = now_ms() - t0;
        if (d < nvec) nvec = d;
        t0 = now_ms();
        gfx_nebula_scalar(pic, W, W, H, 0x40303060u, 0x60101030u, 6, 1u);
        d = now_ms() - t0;
        if (d < nsca) nsca = d;
    }

    free(pic); free(tmp); free(sums);

    if (failures) {
        printf("FAIL: %u of %u checks on the blur.\n", failures, checks);
        return 1;
    }

    printf("PASS: %u checks on the blur (eight at a time the scalar loop's to "
           "the bit, in place and not, at 8 sizes and 5 radii; within one of "
           "the true average; a plain colour back exactly at every radius to "
           "%u; nothing written past a row; 0 and past the most a copy; the "
           "nebula eight at a time the scalar loop's, its gradient exact, its "
           "grain bounded, grey and the same every time). "
           "%ux%u, three passes at radius %u: %.1f ms eight at a time, %.1f "
           "one at a time; the nebula %.1f ms, %.1f one at a time.\n",
           checks, GFX_BLUR_MOST, W, H, R, vec, sca, nvec, nsca);
    return 0;
}
