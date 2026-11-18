/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A picture blurred by box passes. `blur.h` says what for.
 *
 * **One pass is two**: across each row into `tmp`, then down each column
 * out of it. Each keeps a running sum per channel - the pixel entering the
 * window added, the one leaving it taken away - so a pass costs the same
 * at every radius: a few adds and one multiply a channel a pixel.
 *
 * **In the vector unit**, in GCC's own vector types as `pack.c` and
 * `gfx.c`'s blend are written - NEON on AArch64, SSE2 on x86-64, the
 * compiler choosing - with the scalar loop as the tail and the reference
 * (`tools/test_blur.c` holds them equal to the bit). Down the columns,
 * eight at a time: eight neighbouring columns sit side by side in a row, so
 * a lane load is one load, the rows are walked in order and the columns'
 * sums kept between them as four planes. Across a row, a pixel's four
 * channels are the four lanes. Eight rows as eight lanes was tried first
 * and lost on NEON: each lane's pixel loaded on its own, a gather.
 *
 * **Measured on this Mac's cores** (`tools/test_blur.c` prints it): the
 * screen, 1920x1080, three passes at radius 8, 9.8 ms against 10.4 a pixel
 * at a time natively - Apple's compiler vectorises the scalar loop by
 * itself, which GCC freestanding is not counted on to do - and 10.5 against
 * 16.5 through Rosetta. Once a wallpaper, never a frame.
 *
 * **The rounding.** A channel's average is `sum / d`, `d = 2r + 1`, and a
 * vector unit has no divide. So `m = round(2^23 / d)` and the average is
 * `(sum * m + 2^22) >> 23`. For a plain colour `c` the sum is `c * d`, and
 * `c * d * m` is within `c * d / 2` of `c * 2^23` - at most 255 * 511 / 2,
 * well inside the 2^22 the rounding allows - so `c` comes back exactly; and
 * `sum * m` is about `255 * 2^23` whatever `d` is, so with the rounding it
 * stays under 2^32 for every radius to `GFX_BLUR_MOST`. Elsewhere it is
 * within one of the true average (also held by the test).
 */

#include <stddef.h>
#include <string.h>

#include "blur.h"

/*
 * The lanes load and store through `__builtin_memcpy` of a vector's size,
 * which GCC makes one instruction: plain `memcpy` under `-ffreestanding` is
 * a call (`pack.c`, `roadmap.md` 6zz h).
 */
typedef uint32_t u32x8 __attribute__((vector_size(32)));

#define LANES 8u

static inline unsigned clamp(long i, unsigned n)
{
    if (i < 0) return 0;
    if ((unsigned long)i >= n) return n - 1;
    return (unsigned)i;
}

static inline uint32_t mean(uint32_t sum, uint32_t m)
{
    return (sum * m + (1u << 22)) >> 23;
}

static inline uint32_t reciprocal(unsigned r)
{
    uint32_t d = 2u * r + 1u;

    return ((1u << 23) + d / 2u) / d;
}

/* ---- a pixel at a time ------------------------------------------------ */

/* One row across, `w` pixels from `in` to `out`. */
static void across_scalar(const uint32_t *in, uint32_t *out, unsigned w,
                          unsigned r, uint32_t m)
{
    uint32_t s[4] = { 0, 0, 0, 0 };
    long k;
    unsigned x, c;

    for (k = -(long)r; k <= (long)r; k++) {
        uint32_t p = in[clamp(k, w)];

        for (c = 0; c < 4; c++) s[c] += (p >> (8 * c)) & 0xFFu;
    }

    for (x = 0; x < w; x++) {
        uint32_t enter = in[clamp((long)x + r + 1, w)];
        uint32_t leave = in[clamp((long)x - r, w)];
        uint32_t o = 0;

        for (c = 0; c < 4; c++) {
            o |= mean(s[c], m) << (8 * c);
            s[c] += ((enter >> (8 * c)) & 0xFFu) - ((leave >> (8 * c)) & 0xFFu);
        }

        out[x] = o;
    }
}

/*
 * The sums down the columns live in `sums` as four runs of `w`, one a
 * channel, so eight neighbouring columns' sums of a channel are contiguous
 * and the vector loop loads them as one. Started here for every column.
 */
static void down_start(const uint32_t *in, unsigned w, unsigned h,
                       unsigned r, uint32_t *sums)
{
    unsigned x, c;
    long k;

    for (x = 0; x < 4u * w; x++) sums[x] = 0;

    for (k = -(long)r; k <= (long)r; k++) {
        const uint32_t *row = in + (size_t)clamp(k, h) * w;

        for (x = 0; x < w; x++) {
            for (c = 0; c < 4; c++) sums[c * w + x] += (row[x] >> (8 * c)) & 0xFFu;
        }
    }
}

/* Columns `x0` to `x1` of row `y` down, from `in` (rows `w` apart) to `o`. */
static void down_scalar(const uint32_t *in, uint32_t *o, unsigned w,
                        unsigned h, unsigned y, unsigned x0, unsigned x1,
                        unsigned r, uint32_t m, uint32_t *sums)
{
    const uint32_t *enter = in + (size_t)clamp((long)y + r + 1, h) * w;
    const uint32_t *leave = in + (size_t)clamp((long)y - r, h) * w;
    unsigned x, c;

    for (x = x0; x < x1; x++) {
        uint32_t v = 0;

        for (c = 0; c < 4; c++) {
            uint32_t *s = sums + c * w + x;

            v |= mean(*s, m) << (8 * c);
            *s += ((enter[x] >> (8 * c)) & 0xFFu) - ((leave[x] >> (8 * c)) & 0xFFu);
        }

        o[x] = v;
    }
}

/* ---- eight at a time -------------------------------------------------- */

/*
 * One row across, a pixel's four channels as the four lanes of a vector:
 * its bytes widened to 32 bits, summed, and narrowed back - no gather, and
 * the same arithmetic as the scalar loop lane for lane.
 */
typedef uint32_t u32x4 __attribute__((vector_size(16)));
typedef uint8_t  u8x4  __attribute__((vector_size(4)));

static inline u32x4 widen(uint32_t p)
{
    u8x4 b;

    __builtin_memcpy(&b, &p, sizeof(b));
    return __builtin_convertvector(b, u32x4);
}

static void across4(const uint32_t *in, uint32_t *out, unsigned w,
                    unsigned r, uint32_t m)
{
    u32x4 s = { 0, 0, 0, 0 };
    long k;
    unsigned x;

    for (k = -(long)r; k <= (long)r; k++) s += widen(in[clamp(k, w)]);

    for (x = 0; x < w; x++) {
        u32x4 v = (s * m + (1u << 22)) >> 23;
        u8x4 b = __builtin_convertvector(v, u8x4);

        __builtin_memcpy(out + x, &b, sizeof(b));
        s += widen(in[clamp((long)x + r + 1, w)]) - widen(in[clamp((long)x - r, w)]);
    }
}

/* Row `y` down, eight columns at a time and the rest one at a time. */
static void down8(const uint32_t *in, uint32_t *o, unsigned w, unsigned h,
                  unsigned y, unsigned r, uint32_t m, uint32_t *sums)
{
    const uint32_t *enter = in + (size_t)clamp((long)y + r + 1, h) * w;
    const uint32_t *leave = in + (size_t)clamp((long)y - r, h) * w;
    unsigned x;

    for (x = 0; x + LANES <= w; x += LANES) {
        u32x8 e, l, s0, s1, s2, s3, v;

        __builtin_memcpy(&e, enter + x, sizeof(e));
        __builtin_memcpy(&l, leave + x, sizeof(l));
        __builtin_memcpy(&s0, sums + x, sizeof(s0));
        __builtin_memcpy(&s1, sums + w + x, sizeof(s1));
        __builtin_memcpy(&s2, sums + 2u * w + x, sizeof(s2));
        __builtin_memcpy(&s3, sums + 3u * w + x, sizeof(s3));

        v = ((s0 * m + (1u << 22)) >> 23)
          | ((s1 * m + (1u << 22)) >> 23) << 8
          | ((s2 * m + (1u << 22)) >> 23) << 16
          | ((s3 * m + (1u << 22)) >> 23) << 24;

        __builtin_memcpy(o + x, &v, sizeof(v));

        s0 += (e & 0xFFu) - (l & 0xFFu);
        s1 += ((e >> 8) & 0xFFu) - ((l >> 8) & 0xFFu);
        s2 += ((e >> 16) & 0xFFu) - ((l >> 16) & 0xFFu);
        s3 += (e >> 24) - (l >> 24);

        __builtin_memcpy(sums + x, &s0, sizeof(s0));
        __builtin_memcpy(sums + w + x, &s1, sizeof(s1));
        __builtin_memcpy(sums + 2u * w + x, &s2, sizeof(s2));
        __builtin_memcpy(sums + 3u * w + x, &s3, sizeof(s3));
    }

    down_scalar(in, o, w, h, y, x, w, r, m, sums);
}

/* ---- the passes -------------------------------------------------------- */

static void copy(const uint32_t *src, uint32_t *dst, size_t pitch,
                 unsigned w, unsigned h)
{
    unsigned y;

    if (src == dst) return;

    for (y = 0; y < h; y++) {
        __builtin_memmove(dst + (size_t)y * pitch, src + (size_t)y * pitch,
                          (size_t)w * 4u);
    }
}

void gfx_blur_scalar(const uint32_t *src, uint32_t *dst, size_t pitch,
                     unsigned w, unsigned h, unsigned r, unsigned passes,
                     uint32_t *tmp, uint32_t *sums)
{
    uint32_t m = reciprocal(r);
    unsigned n, y;

    if (w == 0 || h == 0) return;

    if (r == 0 || r > GFX_BLUR_MOST || passes == 0) {
        copy(src, dst, pitch, w, h);
        return;
    }

    for (n = 0; n < passes; n++) {
        const uint32_t *from = n == 0 ? src : dst;

        for (y = 0; y < h; y++) {
            across_scalar(from + (size_t)y * pitch, tmp + (size_t)y * w, w, r, m);
        }

        down_start(tmp, w, h, r, sums);

        for (y = 0; y < h; y++) {
            down_scalar(tmp, dst + (size_t)y * pitch, w, h, y, 0, w, r, m, sums);
        }
    }
}

void gfx_blur(const uint32_t *src, uint32_t *dst, size_t pitch,
              unsigned w, unsigned h, unsigned r, unsigned passes,
              uint32_t *tmp, uint32_t *sums)
{
    uint32_t m = reciprocal(r);
    unsigned n, y;

    if (w == 0 || h == 0) return;

    if (r == 0 || r > GFX_BLUR_MOST || passes == 0) {
        copy(src, dst, pitch, w, h);
        return;
    }

    for (n = 0; n < passes; n++) {
        const uint32_t *from = n == 0 ? src : dst;

        for (y = 0; y < h; y++) {
            across4(from + (size_t)y * pitch, tmp + (size_t)y * w, w, r, m);
        }

        down_start(tmp, w, h, r, sums);

        for (y = 0; y < h; y++) {
            down8(tmp, dst + (size_t)y * pitch, w, h, y, r, m, sums);
        }
    }
}

/* ---- the nebula ------------------------------------------------------- */

/*
 * The grain's number for a pixel: a hash of where it is, so the same
 * wallpaper is given the same dust every time and a pixel's grain is
 * nobody's state. Spread over [-grain, grain] by a multiply rather than a
 * remainder, which a vector does not have.
 */
static inline uint32_t dust(uint32_t x, uint32_t y, uint32_t seed)
{
    uint32_t h = (x * 0x9E3779B1u) ^ (y * 0x85EBCA77u + seed);

    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

/* A row's colour: `top` to `bottom`, alpha too, at `y` of `h`, rounded. */
static uint32_t nebula_row(uint32_t top, uint32_t bottom, unsigned y, unsigned h)
{
    uint32_t out = 0;
    unsigned k, d = h > 1 ? h - 1 : 1;

    for (k = 0; k < 32; k += 8) {
        uint32_t a = (top >> k) & 0xFFu, b = (bottom >> k) & 0xFFu;

        out |= ((a * (d - y) + b * y + d / 2) / d) << k;
    }

    return out;
}

/* One channel: `c` at alpha `a` over `v`, divided by 255 exactly, and the
 * grain `n` added and held to a byte. */
static inline int32_t nebula_channel(int32_t v, int32_t c, int32_t a, int32_t n)
{
    int32_t t = c * a + v * (255 - a) + 128;
    int32_t o = ((t + (t >> 8)) >> 8) + n;

    return o < 0 ? 0 : o > 255 ? 255 : o;
}

static void nebula_px(uint32_t *row, unsigned from, unsigned to, unsigned y,
                      uint32_t c, unsigned grain, uint32_t seed)
{
    int32_t a = (int32_t)(c >> 24);
    unsigned x;

    for (x = from; x < to; x++) {
        uint32_t p = row[x];
        int32_t n = (int32_t)(((dust(x, y, seed) >> 16) * (2u * grain + 1u)) >> 16)
                  - (int32_t)grain;

        row[x] = (p & 0xFF000000u)
               | (uint32_t)nebula_channel((int32_t)((p >> 16) & 0xFFu),
                                          (int32_t)((c >> 16) & 0xFFu), a, n) << 16
               | (uint32_t)nebula_channel((int32_t)((p >> 8) & 0xFFu),
                                          (int32_t)((c >> 8) & 0xFFu), a, n) << 8
               | (uint32_t)nebula_channel((int32_t)(p & 0xFFu),
                                          (int32_t)(c & 0xFFu), a, n);
    }
}

void gfx_nebula_scalar(uint32_t *pixels, size_t pitch, unsigned w, unsigned h,
                       uint32_t top, uint32_t bottom, unsigned grain, uint32_t seed)
{
    unsigned y;

    if (grain > GFX_NEBULA_GRAIN_MOST) grain = GFX_NEBULA_GRAIN_MOST;

    for (y = 0; y < h; y++) {
        nebula_px(pixels + (size_t)y * pitch, 0, w, y,
                  nebula_row(top, bottom, y, h), grain, seed);
    }
}

typedef int32_t i32x8 __attribute__((vector_size(32)));

/* The same channel, eight pixels at a time, in place in `v`: the clamp is
 * masks, since a vector in C has no `?:`. Through pointers, because a
 * 32-byte vector passed or returned by value changes the ABI on an x86
 * without AVX, which the compiler refuses. */
static inline void channel8(i32x8 *v, int32_t c, int32_t a, const i32x8 *n)
{
    i32x8 t = c * a + *v * (255 - a) + 128;
    i32x8 o = ((t + (t >> 8)) >> 8) + *n;
    i32x8 over;

    o &= ~(o >> 31);
    over = o > 255;
    *v = (o & ~over) | (255 & over);
}

void gfx_nebula(uint32_t *pixels, size_t pitch, unsigned w, unsigned h,
                uint32_t top, uint32_t bottom, unsigned grain, uint32_t seed)
{
    static const u32x8 step = { 0, 1, 2, 3, 4, 5, 6, 7 };
    unsigned x, y;

    if (grain > GFX_NEBULA_GRAIN_MOST) grain = GFX_NEBULA_GRAIN_MOST;

    for (y = 0; y < h; y++) {
        uint32_t *row = pixels + (size_t)y * pitch;
        uint32_t c = nebula_row(top, bottom, y, h);
        int32_t a = (int32_t)(c >> 24);
        int32_t cr = (int32_t)((c >> 16) & 0xFFu);
        int32_t cg = (int32_t)((c >> 8) & 0xFFu);
        int32_t cb = (int32_t)(c & 0xFFu);

        for (x = 0; x + LANES <= w; x += LANES) {
            u32x8 p, hx, o;
            i32x8 n, r, g, b;

            __builtin_memcpy(&p, row + x, sizeof(p));

            hx = ((step + x) * 0x9E3779B1u) ^ (y * 0x85EBCA77u + seed);
            hx ^= hx >> 15;
            hx *= 0x2C1B3C6Du;
            hx ^= hx >> 12;
            hx *= 0x297A2D39u;
            hx ^= hx >> 15;
            n = (i32x8)(((hx >> 16) * (2u * grain + 1u)) >> 16) - (int32_t)grain;

            r = (i32x8)((p >> 16) & 0xFFu);
            g = (i32x8)((p >> 8) & 0xFFu);
            b = (i32x8)(p & 0xFFu);
            channel8(&r, cr, a, &n);
            channel8(&g, cg, a, &n);
            channel8(&b, cb, a, &n);
            o = (p & 0xFF000000u) | (u32x8)r << 16 | (u32x8)g << 8 | (u32x8)b;

            __builtin_memcpy(row + x, &o, sizeof(o));
        }

        nebula_px(row, x, w, y, c, grain, seed);
    }
}
