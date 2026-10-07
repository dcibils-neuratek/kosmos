/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `user/kits/gfx/zrle.c`, held to a ZRLE decoder written here from RFC 6143
 * 7.7.6 rather than from the encoder: every tile of every picture decoded
 * back and compared, pixel for pixel, with `gfx_pack_pixel` of the source -
 * in eight pixel formats, including both three-byte CPIXELs, 565 in either
 * byte order and eight bits - over pictures made to reach each of the five
 * subencodings, at sizes that are not whole tiles. And the choices: a flat
 * tile is one colour, a two-colour one is a packed palette of one bit, and
 * a buffer smaller than the bound is refused before anything is written.
 *
 * Prints how small a desktop-like picture comes out before zlib, which is
 * the number that says whether this was worth doing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zrle.h"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    printf("not ok - "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* --- the decoder ------------------------------------------------------ */

struct reader { const uint8_t *p, *end; };

static int take(struct reader *r, uint8_t *b)
{
    if (r->p >= r->end) {
        return 0;
    }

    *b = *r->p++;
    return 1;
}

static int cpixel(struct reader *r, unsigned cp, int big, int high, uint32_t *v)
{
    uint8_t b[4];
    unsigned i;

    for (i = 0; i < cp; i++) {
        if (!take(r, &b[i])) {
            return 0;
        }
    }

    *v = 0;

    for (i = 0; i < cp; i++) {
        unsigned at = big ? cp - 1 - i : i;

        *v |= (uint32_t)b[i] << (8 * at);
    }

    if (cp == 3 && high) {
        *v <<= 8;
    }

    return 1;
}

static int length(struct reader *r, size_t *n)
{
    uint8_t b;

    *n = 1;

    do {
        if (!take(r, &b)) {
            return 0;
        }

        *n += b;
    } while (b == 255);

    return 1;
}

/* The rectangle back as values, or a sentence for what was wrong. */
static const char *decode(const uint8_t *data, size_t bytes, unsigned w, unsigned h,
                          unsigned cp, int big, int high, uint32_t *out, int *kinds)
{
    struct reader r = { data, data + bytes };
    unsigned ty, tx;

    for (ty = 0; ty < h; ty += 64) {
        unsigned th = h - ty < 64 ? h - ty : 64;

        for (tx = 0; tx < w; tx += 64) {
            unsigned tw = w - tx < 64 ? w - tx : 64;
            uint32_t pal[128];
            uint8_t sub;
            size_t n = (size_t)tw * th, i = 0;
            unsigned k;

            if (!take(&r, &sub)) {
                return "ran out before a tile";
            }

            kinds[sub == 0 ? 0 : sub == 1 ? 1 : sub <= 16 ? 2 : sub == 128 ? 3 : 4]++;

#define AT(i) out[(size_t)(ty + (i) / tw) * w + tx + (i) % tw]

            if (sub == 0) {
                for (i = 0; i < n; i++) {
                    if (!cpixel(&r, cp, big, high, &AT(i))) return "raw ran out";
                }
            } else if (sub == 1) {
                uint32_t v;

                if (!cpixel(&r, cp, big, high, &v)) return "solid ran out";

                for (i = 0; i < n; i++) AT(i) = v;
            } else if (sub >= 2 && sub <= 16) {
                unsigned bits = sub <= 2 ? 1 : sub <= 4 ? 2 : 4, row, col;

                for (k = 0; k < sub; k++) {
                    if (!cpixel(&r, cp, big, high, &pal[k])) return "palette ran out";
                }

                for (row = 0; row < th; row++) {
                    unsigned have = 0;
                    uint8_t byte = 0;

                    for (col = 0; col < tw; col++) {
                        unsigned idx;

                        if (have == 0) {
                            if (!take(&r, &byte)) return "packed ran out";
                            have = 8;
                        }

                        idx = (byte >> (have - bits)) & ((1u << bits) - 1u);
                        have -= bits;

                        if (idx >= sub) return "a packed index past the palette";

                        out[(size_t)(ty + row) * w + tx + col] = pal[idx];
                    }
                }
            } else if (sub == 128) {
                while (i < n) {
                    uint32_t v;
                    size_t run, j;

                    if (!cpixel(&r, cp, big, high, &v) || !length(&r, &run))
                        return "plain RLE ran out";
                    if (i + run > n) return "a plain run past the tile";

                    for (j = 0; j < run; j++) AT(i + j) = v;

                    i += run;
                }
            } else if (sub >= 130) {
                unsigned size = sub - 128;

                for (k = 0; k < size; k++) {
                    if (!cpixel(&r, cp, big, high, &pal[k])) return "RLE palette ran out";
                }

                while (i < n) {
                    uint8_t b;
                    size_t run = 1, j;

                    if (!take(&r, &b)) return "palette RLE ran out";
                    if (b & 128 && !length(&r, &run)) return "a run's length ran out";
                    if ((b & 127) >= size) return "an RLE index past the palette";
                    if (i + run > n) return "a palette run past the tile";

                    for (j = 0; j < run; j++) AT(i + j) = pal[b & 127];

                    i += run;
                }
            } else {
                return "a subencoding ZRLE does not have";
            }
#undef AT
        }
    }

    return r.p == r.end ? NULL : "bytes left over after the last tile";
}

/* --- pictures ---------------------------------------------------------- */

static uint32_t seed = 12345;

static uint32_t rnd(void)
{
    seed = seed * 1103515245u + 12345u;
    return seed >> 8;
}

/* A desktop, roughly: flat ground, panels, lines of "text", a photo corner. */
static void desktop(uint32_t *px, unsigned w, unsigned h)
{
    unsigned x, y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t c = 0xFF2E5CB8u;                       /* the ground */

            if (x > 40 && x < w / 2 && y > 30 && y < h - 40) {
                c = 0xFFF4F2EFu;                            /* a window */

                if ((y / 18) % 2 == 0 && (x * 7 + y * 3) % 11 < 3) {
                    c = 0xFF1F2328u;                        /* its text */
                }
            }

            if (x > w * 3 / 4 && y > h * 3 / 4) {
                c = 0xFF000000u | (rnd() & 0xFFFFFFu);      /* a photo */
            }

            px[(size_t)y * w + x] = c;
        }
    }
}

enum { FLAT, TWO, TEN, HUNDRED, NOISE, DESK, KINDS };

static void picture(int kind, uint32_t *px, unsigned w, unsigned h)
{
    static const uint32_t ten[10] = { 0xFF000000, 0xFFFFFFFF, 0xFF102030, 0xFFAA0000,
        0xFF00AA00, 0xFF0000AA, 0xFF808080, 0xFFC0C0C0, 0xFF123456, 0xFFFEDCBA };
    size_t i, n = (size_t)w * h;

    if (kind == DESK) {
        desktop(px, w, h);
        return;
    }

    for (i = 0; i < n; i++) {
        switch (kind) {
        case FLAT:    px[i] = 0xFF336699u; break;
        case TWO:     px[i] = (rnd() & 1) ? 0xFFFFFFFFu : 0xFF000000u; break;
        case TEN:     px[i] = ten[(i / 7) % 10]; break;
        case HUNDRED: px[i] = 0xFF000000u | (((i / 23) % 100) * 0x020305u); break;
        default:      px[i] = 0xFF000000u | (rnd() & 0xFFFFFFu); break;
        }
    }
}

struct fmt { const char *name; struct gfx_pack_format f; unsigned depth; unsigned cp; int high; };

int main(void)
{
    static const struct fmt formats[] = {
        { "32 little, red at 16 (TigerVNC's)", { 32, false, 255, 255, 255, 16, 8, 0 }, 24, 3, 0 },
        { "32 big, red at 16",                  { 32, true,  255, 255, 255, 16, 8, 0 }, 24, 3, 0 },
        { "32 little, red at 24 (high bytes)",  { 32, false, 255, 255, 255, 24, 16, 8 }, 24, 3, 1 },
        { "32 big, blue at 24 (high bytes)",    { 32, true,  255, 255, 255, 8, 16, 24 }, 24, 3, 1 },
        { "32 of depth 32 (four bytes)",        { 32, false, 255, 255, 255, 16, 8, 0 }, 32, 4, 0 },
        { "565 little",                         { 16, false, 31, 63, 31, 11, 5, 0 }, 16, 2, 0 },
        { "565 big",                            { 16, true,  31, 63, 31, 11, 5, 0 }, 16, 2, 0 },
        { "8, bgr233",                          { 8,  false, 7, 7, 3, 0, 3, 6 }, 8, 1, 0 },
    };
    static const char *kind_name[KINDS] = { "flat", "two colours", "ten colours",
                                            "a hundred colours", "noise", "a desktop" };
    static const unsigned sizes[][2] = { { 64, 64 }, { 200, 130 }, { 1, 1 }, { 65, 3 }, { 333, 77 } };
    size_t f, k, s;
    int total_kinds[5] = { 0 };

    for (f = 0; f < sizeof formats / sizeof formats[0]; f++) {
        const struct fmt *ft = &formats[f];

        CHECK(gfx_zrle_cpixel(&ft->f, ft->depth) == ft->cp,
              "%s: a CPIXEL of %u bytes, not %u", ft->name,
              gfx_zrle_cpixel(&ft->f, ft->depth), ft->cp);

        for (k = 0; k < KINDS; k++) {
            for (s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
                unsigned w = sizes[s][0], h = sizes[s][1];
                /* Drawn into a wider surface, so the pitch is not the width. */
                size_t pitch = w + 13, n = (size_t)w * h, i;
                uint32_t *src = calloc(pitch * h, 4), *dense = malloc(n * 4);
                uint32_t *back = calloc(n, 4);
                size_t cap = gfx_zrle_bound(w, h), got;
                uint8_t *out = malloc(cap);
                const char *why;
                int kinds[5] = { 0 }, wrong = 0;

                picture((int)k, dense, w, h);

                for (i = 0; i < n; i++) {
                    src[(i / w) * pitch + i % w] = dense[i];
                }

                got = gfx_zrle_rect(src, pitch, w, h, &ft->f, ft->depth, out, cap);
                CHECK(got > 0 && got <= cap, "%s, %s, %ux%u: %zu bytes of %zu",
                      ft->name, kind_name[k], w, h, got, cap);

                why = decode(out, got, w, h, ft->cp, ft->f.big, ft->high, back, kinds);
                CHECK(why == NULL, "%s, %s, %ux%u: %s", ft->name, kind_name[k], w, h,
                      why ? why : "");

                for (i = 0; why == NULL && i < n; i++) {
                    if (back[i] != gfx_pack_pixel(dense[i], &ft->f)) {
                        wrong++;
                    }
                }

                CHECK(wrong == 0, "%s, %s, %ux%u: %d pixels decoded wrong",
                      ft->name, kind_name[k], w, h, wrong);

                for (i = 0; i < 5; i++) total_kinds[i] += kinds[i];

                if (k == FLAT && w == 64 && h == 64) {
                    CHECK(got == 1 + ft->cp, "%s: a flat tile is %zu bytes, not one "
                          "colour's %u", ft->name, got, 1 + ft->cp);
                }

                if (k == TWO && w == 64 && h == 64) {
                    CHECK(got == 1 + 2 * ft->cp + 64 * 8, "%s: a two-colour tile is %zu "
                          "bytes, not a palette of two and one bit a pixel", ft->name, got);
                }

                if (s == 0 && k == FLAT) {
                    CHECK(gfx_zrle_rect(src, pitch, w, h, &ft->f, ft->depth, out, cap - 1) == 0,
                          "%s: a buffer one byte short was not refused", ft->name);
                }

                free(src); free(dense); free(back); free(out);
            }
        }
    }

    CHECK(total_kinds[0] > 0 && total_kinds[1] > 0 && total_kinds[2] > 0
          && total_kinds[3] > 0 && total_kinds[4] > 0,
          "not every subencoding was reached: raw %d, solid %d, packed %d, "
          "plain RLE %d, palette RLE %d", total_kinds[0], total_kinds[1],
          total_kinds[2], total_kinds[3], total_kinds[4]);

    /* How small a 1720x1440 desktop is, in TigerVNC's format, before zlib. */
    {
        unsigned w = 1720, h = 1440;
        uint32_t *px = malloc((size_t)w * h * 4);
        size_t cap = gfx_zrle_bound(w, h), got;
        uint8_t *out = malloc(cap);

        seed = 7;
        desktop(px, w, h);
        got = gfx_zrle_rect(px, w, w, h, &formats[0].f, 24, out, cap);
        printf("a %ux%u desktop: %zu bytes raw, %zu as tiles (%.1f%%), before zlib\n",
               w, h, (size_t)w * h * 4, got, 100.0 * got / ((double)w * h * 4));
        free(px); free(out);
    }

    if (failures) {
        printf("FAIL: %d checks\n", failures);
        return 1;
    }

    printf("PASS: ZRLE's tiles decoded back exactly in %zu formats, every "
           "subencoding reached (raw %d, solid %d, packed %d, plain RLE %d, "
           "palette RLE %d)\n", sizeof formats / sizeof formats[0], total_kinds[0],
           total_kinds[1], total_kinds[2], total_kinds[3], total_kinds[4]);
    return 0;
}
