/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A rectangle as ZRLE's tiles. `zrle.h` says what for.
 *
 * **One pass to look, one to write.** The first pass turns each pixel into
 * the viewer's value (`gfx_pack_pixel`, remembered for the last colour seen,
 * since a desktop's pixels come in long runs of one), and counts what each
 * way of writing the tile would cost: its palette, while there are 127
 * colours or fewer, and its runs, which in ZRLE carry on from the end of one
 * row to the start of the next. The second writes the cheapest. Nothing is
 * guessed: every size is the exact number of bytes that way would write.
 *
 * Tiles are written left to right, top to bottom, as the RFC orders them.
 */

#include <string.h>

#include "zrle.h"

#define TILE      GFX_ZRLE_TILE
#define MOST_RLE  127            /* a palette RLE's colours */
#define MOST_PACK 16             /* a packed palette's */
#define SLOTS     256            /* the palette's hash, twice the most */

/* Subencodings (RFC 6143, 7.7.6). */
#define RAW        0
#define SOLID      1
#define PLAIN_RLE  128

unsigned gfx_zrle_cpixel(const struct gfx_pack_format *f, unsigned depth)
{
    uint32_t used;

    if (f->bpp != 32) {
        return f->bpp / 8u;
    }

    used = (f->rmax << f->rshift) | (f->gmax << f->gshift)
         | (f->bmax << f->bshift);

    if (depth <= 24 && ((used & 0xFF000000u) == 0 || (used & 0xFFu) == 0)) {
        return 3;
    }

    return 4;
}

size_t gfx_zrle_bound(unsigned w, unsigned h)
{
    size_t tiles = (size_t)((w + TILE - 1) / TILE) * ((h + TILE - 1) / TILE);

    /* At worst every tile raw, four bytes a pixel, and its subencoding. */
    return tiles + (size_t)w * h * 4u;
}

/* One value as a CPIXEL, `cp` bytes, in the format's byte order. */
static uint8_t *put_cpixel(uint8_t *o, uint32_t v, unsigned cp,
                           const struct gfx_pack_format *f, bool high)
{
    if (cp == 1) {
        *o++ = (uint8_t)v;
        return o;
    }

    if (cp == 2) {
        if (f->big) {
            *o++ = (uint8_t)(v >> 8);
            *o++ = (uint8_t)v;
        } else {
            *o++ = (uint8_t)v;
            *o++ = (uint8_t)(v >> 8);
        }

        return o;
    }

    if (cp == 3) {
        /* The three bytes the colour is in: the low three, or the high. */
        if (high) {
            v >>= 8;
        }

        if (f->big) {
            *o++ = (uint8_t)(v >> 16);
            *o++ = (uint8_t)(v >> 8);
            *o++ = (uint8_t)v;
        } else {
            *o++ = (uint8_t)v;
            *o++ = (uint8_t)(v >> 8);
            *o++ = (uint8_t)(v >> 16);
        }

        return o;
    }

    if (f->big) {
        *o++ = (uint8_t)(v >> 24);
        *o++ = (uint8_t)(v >> 16);
        *o++ = (uint8_t)(v >> 8);
        *o++ = (uint8_t)v;
    } else {
        *o++ = (uint8_t)v;
        *o++ = (uint8_t)(v >> 8);
        *o++ = (uint8_t)(v >> 16);
        *o++ = (uint8_t)(v >> 24);
    }

    return o;
}

/* A run's length as RLE writes it: (length - 1) in 255s and what is left. */
static uint8_t *put_length(uint8_t *o, size_t length)
{
    size_t n = length - 1;

    while (n >= 255) {
        *o++ = 255;
        n -= 255;
    }

    *o++ = (uint8_t)n;
    return o;
}

static size_t length_bytes(size_t length)
{
    return (length - 1) / 255u + 1u;
}

/* The palette so far: colours in the order met, and a hash to find them. */
struct palette {
    uint32_t colour[MOST_RLE];
    unsigned n;
    bool     over;              /* more than MOST_RLE met */
    uint32_t key[SLOTS];
    int16_t  at[SLOTS];         /* -1 for an empty slot */
};

static int palette_find(const struct palette *p, uint32_t v)
{
    unsigned s = (v * 2654435761u) >> 24;      /* SLOTS is 256 */

    while (p->at[s] >= 0) {
        if (p->key[s] == v) {
            return p->at[s];
        }

        s = (s + 1u) & (SLOTS - 1u);
    }

    return -1;
}

static void palette_add(struct palette *p, uint32_t v)
{
    unsigned s;

    if (p->over || palette_find(p, v) >= 0) {
        return;
    }

    if (p->n == MOST_RLE) {
        p->over = true;
        return;
    }

    s = (v * 2654435761u) >> 24;

    while (p->at[s] >= 0) {
        s = (s + 1u) & (SLOTS - 1u);
    }

    p->key[s] = v;
    p->at[s] = (int16_t)p->n;
    p->colour[p->n++] = v;
}

/* Bits a pixel's index takes in a packed palette of `n` colours. */
static unsigned index_bits(unsigned n)
{
    return n <= 2 ? 1u : n <= 4 ? 2u : 4u;
}

static uint8_t *tile(const uint32_t *src, size_t pitch, unsigned w, unsigned h,
                     const struct gfx_pack_format *f, unsigned cp, bool high,
                     uint8_t *o)
{
    static uint32_t value[TILE * TILE];
    static struct palette pal;
    size_t n = (size_t)w * h, i;
    size_t runs_plain = 0, runs_pal = 0, run = 0;
    uint32_t last_src = 0, last_value = 0;
    bool have_last = false;
    size_t raw, plain, packed = (size_t)-1, rle = (size_t)-1, best;
    unsigned row, col;

    pal.n = 0;
    pal.over = false;
    memset(pal.at, 0xFF, sizeof pal.at);

    /* Look: every pixel as the viewer's value, the palette, the runs. */
    for (row = 0; row < h; row++) {
        const uint32_t *line = src + (size_t)row * pitch;

        for (col = 0; col < w; col++) {
            uint32_t p = line[col] & 0x00FFFFFFu;

            if (!have_last || p != last_src) {
                last_src = p;
                last_value = gfx_pack_pixel(p, f);
                have_last = true;
            }

            value[(size_t)row * w + col] = last_value;
            palette_add(&pal, last_value);
        }
    }

    for (i = 0; i < n; i++) {
        run++;

        if (i + 1 == n || value[i + 1] != value[i]) {
            runs_plain += cp + length_bytes(run);
            runs_pal += (run == 1) ? 1u : 1u + length_bytes(run);
            run = 0;
        }
    }

    if (!pal.over && pal.n == 1) {
        *o++ = SOLID;
        return put_cpixel(o, value[0], cp, f, high);
    }

    raw = n * cp;
    plain = runs_plain;

    if (!pal.over) {
        rle = (size_t)pal.n * cp + runs_pal;

        if (pal.n <= MOST_PACK) {
            packed = (size_t)pal.n * cp
                   + (size_t)h * ((w * index_bits(pal.n) + 7u) / 8u);
        }
    }

    best = raw;

    if (plain < best) {
        best = plain;
    }

    if (rle < best) {
        best = rle;
    }

    if (packed < best) {
        best = packed;
    }

    if (best == packed) {
        unsigned bits = index_bits(pal.n);

        *o++ = (uint8_t)pal.n;

        for (i = 0; i < pal.n; i++) {
            o = put_cpixel(o, pal.colour[i], cp, f, high);
        }

        /* Most significant bits first, each row to a whole byte. */
        for (row = 0; row < h; row++) {
            unsigned byte = 0, used = 0;

            for (col = 0; col < w; col++) {
                unsigned k = (unsigned)palette_find(&pal, value[(size_t)row * w + col]);

                byte = (byte << bits) | k;
                used += bits;

                if (used == 8) {
                    *o++ = (uint8_t)byte;
                    byte = used = 0;
                }
            }

            if (used > 0) {
                *o++ = (uint8_t)(byte << (8 - used));
            }
        }

        return o;
    }

    if (best == rle) {
        *o++ = (uint8_t)(PLAIN_RLE + pal.n);

        for (i = 0; i < pal.n; i++) {
            o = put_cpixel(o, pal.colour[i], cp, f, high);
        }

        for (i = 0, run = 0; i < n; i++) {
            run++;

            if (i + 1 == n || value[i + 1] != value[i]) {
                unsigned k = (unsigned)palette_find(&pal, value[i]);

                if (run == 1) {
                    *o++ = (uint8_t)k;
                } else {
                    *o++ = (uint8_t)(k | 128u);
                    o = put_length(o, run);
                }

                run = 0;
            }
        }

        return o;
    }

    if (best == plain) {
        *o++ = PLAIN_RLE;

        for (i = 0, run = 0; i < n; i++) {
            run++;

            if (i + 1 == n || value[i + 1] != value[i]) {
                o = put_cpixel(o, value[i], cp, f, high);
                o = put_length(o, run);
                run = 0;
            }
        }

        return o;
    }

    *o++ = RAW;

    for (i = 0; i < n; i++) {
        o = put_cpixel(o, value[i], cp, f, high);
    }

    return o;
}

size_t gfx_zrle_rect(const uint32_t *src, size_t pitch, unsigned w, unsigned h,
                     const struct gfx_pack_format *f, unsigned depth,
                     uint8_t *out, size_t cap)
{
    unsigned cp = gfx_zrle_cpixel(f, depth);
    uint32_t used = (f->rmax << f->rshift) | (f->gmax << f->gshift)
                  | (f->bmax << f->bshift);
    bool high = (cp == 3) && (used & 0xFF000000u) != 0;
    uint8_t *o = out;
    unsigned ty, tx;

    if (cap < gfx_zrle_bound(w, h)) {
        return 0;
    }

    for (ty = 0; ty < h; ty += TILE) {
        unsigned th = (h - ty < TILE) ? h - ty : TILE;

        for (tx = 0; tx < w; tx += TILE) {
            unsigned tw = (w - tx < TILE) ? w - tx : TILE;

            o = tile(src + (size_t)ty * pitch + tx, pitch, tw, th, f, cp, high, o);
        }
    }

    return (size_t)(o - out);
}
