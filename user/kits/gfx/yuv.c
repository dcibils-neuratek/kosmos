/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * YUY2 to 0xffRRGGBB. `yuv.h` says what and why.
 *
 * The arithmetic is BT.601's studio-range matrix scaled by 256, the form
 * every integer implementation uses - brightness stretched from 16..235 to
 * 0..255 by 298/256, and the colour differences weighted:
 *
 *   R = (298 C + 409 E + 128) >> 8
 *   G = (298 C - 100 D - 208 E + 128) >> 8
 *   B = (298 C + 516 D + 128) >> 8
 *
 * with C = Y - 16, D = U - 128, E = V - 128, and each clipped to 0..255.
 *
 * **Sixteen pixels at a time on AArch64 and eight on x86-64**, the same
 * sums in 32-bit lanes so the answer is the scalar one to the bit - held
 * so on the Mac by `tools/test_yuv.c`, NEON natively and SSE2 through
 * Rosetta - and the scalar loop for what is left of a row. The sums do not
 * fit sixteen bits (298 x 239 alone is 71,222), which is why the lanes are
 * thirty-two wide and the products widening multiplies; the clip is two
 * saturating narrowings, 32 to 16 bits and 16 to 8, which clamp exactly as
 * the scalar `clip` does.
 */

#include <stddef.h>

#include "yuv.h"

#if defined(__ARM_NEON)
#include <arm_neon.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

static inline uint32_t clip(int x)
{
    return x < 0 ? 0u : (x > 255 ? 255u : (uint32_t)x);
}

uint32_t gfx_yuv_pixel(int y, int u, int v)
{
    int c = 298 * (y - 16) + 128;
    int d = u - 128;
    int e = v - 128;

    return 0xff000000u
         | clip((c + 409 * e) >> 8) << 16
         | clip((c - 100 * d - 208 * e) >> 8) << 8
         | clip((c + 516 * d) >> 8);
}

/* Pairs `from` to `to` of one row, one pair at a time. */
static void row_scalar(uint32_t *row, const uint8_t *src, unsigned width,
                       unsigned from, unsigned to, bool mirror)
{
    const uint8_t *s = src + from * 4u;
    unsigned x;

    for (x = from; x < to; x++, s += 4) {
        int d = s[1] - 128;
        int e = s[3] - 128;
        int r = 409 * e + 128;
        int g = -100 * d - 208 * e + 128;
        int b = 516 * d + 128;
        int c0 = 298 * (s[0] - 16);
        int c1 = 298 * (s[2] - 16);
        uint32_t p0 = 0xff000000u | clip((c0 + r) >> 8) << 16
                    | clip((c0 + g) >> 8) << 8 | clip((c0 + b) >> 8);
        uint32_t p1 = 0xff000000u | clip((c1 + r) >> 8) << 16
                    | clip((c1 + g) >> 8) << 8 | clip((c1 + b) >> 8);

        if (mirror) {
            row[width - 1u - 2u * x] = p0;
            row[width - 2u - 2u * x] = p1;
        } else {
            row[2u * x] = p0;
            row[2u * x + 1u] = p1;
        }
    }
}

void gfx_yuy2_scalar(uint32_t *dst, unsigned long pitch, const uint8_t *src,
                     unsigned width, unsigned height, bool mirror)
{
    unsigned y;

    for (y = 0; y < height; y++) {
        row_scalar((uint32_t *)((uint8_t *)dst + (unsigned long)y * pitch),
                   src + (unsigned long)y * width * 2u, width, 0, width / 2u,
                   mirror);
    }
}

#if defined(__ARM_NEON)

/*
 * One channel for eight pairs' worth of pixels: the pair's shared term
 * (`off`, in two halves of four) added to each brightness, shifted, and
 * narrowed with saturation - to 16 bits, which clamps what is below zero,
 * then to 8, which clamps what is above 255.
 */
static inline uint8x8_t channel(int32x4_t c_lo, int32x4_t c_hi,
                                int32x4_t off_lo, int32x4_t off_hi)
{
    int32x4_t lo = vshrq_n_s32(vaddq_s32(c_lo, off_lo), 8);
    int32x4_t hi = vshrq_n_s32(vaddq_s32(c_hi, off_hi), 8);

    return vqmovn_u16(vcombine_u16(vqmovun_s32(lo), vqmovun_s32(hi)));
}

/*
 * Eight pairs - sixteen pixels - from 32 bytes: `vld4` takes Y0, U, Y1 and
 * V apart in one load, and `vst4` puts B, G, R and A back together in one
 * store, the two brightnesses of a pair zipped into neighbouring pixels.
 * Mirrored, the pairs are reversed in their lanes and the second pixel of
 * each goes first, which is the same sixteen pixels right to left.
 */
static void row_neon(uint32_t *row, const uint8_t *src, unsigned width,
                     unsigned pairs, bool mirror)
{
    const int16x8_t k16 = vdupq_n_s16(16), k128 = vdupq_n_s16(128);
    const int32x4_t round = vdupq_n_s32(128);
    unsigned x;

    for (x = 0; x + 8u <= pairs; x += 8u) {
        uint8x8x4_t in = vld4_u8(src + x * 4u);
        int16x8_t y0 = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(in.val[0])), k16);
        int16x8_t y1 = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(in.val[2])), k16);
        int16x8_t d = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(in.val[1])), k128);
        int16x8_t e = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(in.val[3])), k128);
        int16x4_t d_lo = vget_low_s16(d), d_hi = vget_high_s16(d);
        int16x4_t e_lo = vget_low_s16(e), e_hi = vget_high_s16(e);

        int32x4_t r_lo = vmlal_n_s16(round, e_lo, 409);
        int32x4_t r_hi = vmlal_n_s16(round, e_hi, 409);
        int32x4_t g_lo = vmlal_n_s16(vmlal_n_s16(round, d_lo, -100), e_lo, -208);
        int32x4_t g_hi = vmlal_n_s16(vmlal_n_s16(round, d_hi, -100), e_hi, -208);
        int32x4_t b_lo = vmlal_n_s16(round, d_lo, 516);
        int32x4_t b_hi = vmlal_n_s16(round, d_hi, 516);

        int32x4_t c0_lo = vmull_n_s16(vget_low_s16(y0), 298);
        int32x4_t c0_hi = vmull_n_s16(vget_high_s16(y0), 298);
        int32x4_t c1_lo = vmull_n_s16(vget_low_s16(y1), 298);
        int32x4_t c1_hi = vmull_n_s16(vget_high_s16(y1), 298);

        uint8x8_t r0 = channel(c0_lo, c0_hi, r_lo, r_hi);
        uint8x8_t g0 = channel(c0_lo, c0_hi, g_lo, g_hi);
        uint8x8_t b0 = channel(c0_lo, c0_hi, b_lo, b_hi);
        uint8x8_t r1 = channel(c1_lo, c1_hi, r_lo, r_hi);
        uint8x8_t g1 = channel(c1_lo, c1_hi, g_lo, g_hi);
        uint8x8_t b1 = channel(c1_lo, c1_hi, b_lo, b_hi);
        uint8x16x4_t out;
        uint8x8x2_t z;

        if (mirror) {
            z = vzip_u8(vrev64_u8(b1), vrev64_u8(b0));
            out.val[0] = vcombine_u8(z.val[0], z.val[1]);
            z = vzip_u8(vrev64_u8(g1), vrev64_u8(g0));
            out.val[1] = vcombine_u8(z.val[0], z.val[1]);
            z = vzip_u8(vrev64_u8(r1), vrev64_u8(r0));
            out.val[2] = vcombine_u8(z.val[0], z.val[1]);
            out.val[3] = vdupq_n_u8(0xff);
            vst4q_u8((uint8_t *)(row + width - 16u - 2u * x), out);
        } else {
            z = vzip_u8(b0, b1);
            out.val[0] = vcombine_u8(z.val[0], z.val[1]);
            z = vzip_u8(g0, g1);
            out.val[1] = vcombine_u8(z.val[0], z.val[1]);
            z = vzip_u8(r0, r1);
            out.val[2] = vcombine_u8(z.val[0], z.val[1]);
            out.val[3] = vdupq_n_u8(0xff);
            vst4q_u8((uint8_t *)(row + 2u * x), out);
        }
    }

    row_scalar(row, src, width, x, pairs, mirror);
}

#elif defined(__SSE2__)

/*
 * A 32-bit sum of two 16-bit products for each of four lanes, which is
 * exactly what `pmaddwd` computes: interleave the two operands, and the
 * constants beside them. `a * ka + b * kb`, four pixels at once.
 */
static inline __m128i madd2(__m128i a, __m128i b, short ka, short kb,
                            int high)
{
    __m128i ab = high ? _mm_unpackhi_epi16(a, b) : _mm_unpacklo_epi16(a, b);

    return _mm_madd_epi16(ab, _mm_set_epi16(kb, ka, kb, ka, kb, ka, kb, ka));
}

/* Four lanes and four lanes of a channel, shifted and clamped to 16 bits
 * signed - they are all within it by then - ready for the byte pack. */
static inline __m128i half(__m128i lo, __m128i hi, __m128i round)
{
    lo = _mm_srai_epi32(_mm_add_epi32(lo, round), 8);
    hi = _mm_srai_epi32(_mm_add_epi32(hi, round), 8);
    return _mm_packs_epi32(lo, hi);
}

/*
 * Four pairs - eight pixels - from 16 bytes. SSE2 has no byte shuffle, so
 * the bytes are taken apart with a mask and a shift: the low byte of each
 * 16-bit lane is a brightness, one per pixel and already in order, and the
 * high byte is U, V, U, V, which `pshuflw`/`pshufhw` copy to each pixel of
 * its pair. Then every channel is two `pmaddwd`s, and `packus` clips.
 */
static void row_sse2(uint32_t *row, const uint8_t *src, unsigned width,
                     unsigned pairs, bool mirror)
{
    const __m128i low_byte = _mm_set1_epi16(0x00ff);
    const __m128i k16 = _mm_set1_epi16(16), k128 = _mm_set1_epi16(128);
    const __m128i round = _mm_set1_epi32(128);
    const __m128i zero = _mm_setzero_si128();
    const __m128i alpha = _mm_set1_epi8((char)0xff);
    unsigned x;

    for (x = 0; x + 4u <= pairs; x += 4u) {
        __m128i in = _mm_loadu_si128((const __m128i *)(const void *)
                                     (src + x * 4u));
        __m128i c = _mm_sub_epi16(_mm_and_si128(in, low_byte), k16);
        __m128i uv = _mm_sub_epi16(_mm_srli_epi16(in, 8), k128);
        __m128i d = _mm_shufflehi_epi16(_mm_shufflelo_epi16(uv, 0xA0), 0xA0);
        __m128i e = _mm_shufflehi_epi16(_mm_shufflelo_epi16(uv, 0xF5), 0xF5);

        /* R = 298 C + 409 E; G = 298 C - 208 E - 100 D; B = 298 C + 516 D */
        __m128i r = half(madd2(c, e, 298, 409, 0), madd2(c, e, 298, 409, 1),
                         round);
        __m128i g = half(_mm_add_epi32(madd2(c, e, 298, -208, 0),
                                       madd2(d, zero, -100, 0, 0)),
                         _mm_add_epi32(madd2(c, e, 298, -208, 1),
                                       madd2(d, zero, -100, 0, 1)), round);
        __m128i b = half(madd2(c, d, 298, 516, 0), madd2(c, d, 298, 516, 1),
                         round);

        /* To bytes, clipped, and into B G R A order: eight pixels. */
        __m128i r8 = _mm_packus_epi16(r, r);
        __m128i g8 = _mm_packus_epi16(g, g);
        __m128i b8 = _mm_packus_epi16(b, b);
        __m128i bg = _mm_unpacklo_epi8(b8, g8);
        __m128i ra = _mm_unpacklo_epi8(r8, alpha);
        __m128i p_lo = _mm_unpacklo_epi16(bg, ra);      /* pixels 0-3 */
        __m128i p_hi = _mm_unpackhi_epi16(bg, ra);      /* pixels 4-7 */

        if (mirror) {
            uint32_t *at = row + width - 8u - 2u * x;

            _mm_storeu_si128((__m128i *)(void *)at,
                             _mm_shuffle_epi32(p_hi, 0x1B));
            _mm_storeu_si128((__m128i *)(void *)(at + 4),
                             _mm_shuffle_epi32(p_lo, 0x1B));
        } else {
            _mm_storeu_si128((__m128i *)(void *)(row + 2u * x), p_lo);
            _mm_storeu_si128((__m128i *)(void *)(row + 2u * x + 4u), p_hi);
        }
    }

    row_scalar(row, src, width, x, pairs, mirror);
}

#endif

void gfx_yuy2(uint32_t *dst, unsigned long pitch, const uint8_t *src,
              unsigned width, unsigned height, bool mirror)
{
    unsigned y, pairs = width / 2u;

    for (y = 0; y < height; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)dst + (unsigned long)y * pitch);
        const uint8_t *s = src + (unsigned long)y * width * 2u;

#if defined(__ARM_NEON)
        row_neon(row, s, width, pairs, mirror);
#elif defined(__SSE2__)
        row_sse2(row, s, width, pairs, mirror);
#else
        row_scalar(row, s, width, 0, pairs, mirror);
#endif
    }
}

/*
 * **YUY2 into three planes, 4:2:0** - what an H.264 encoder takes (the Record
 * Kit, `roadmap.md` 6d 8f). Every Y as it came; a U and a V for each two by
 * two pixels, the rounded average of the two rows': `(a + b + 1) >> 1`, which
 * is `vrhaddq_u8` on NEON and `_mm_avg_epu8` on SSE2 exactly - so the vector
 * paths below are the scalar one bit for bit, and `tools/test_yuv.c` holds
 * them to it. An odd last row is its own pair.
 */
static void pair_scalar(uint8_t *y0, uint8_t *y1, uint8_t *u, uint8_t *v,
                        const uint8_t *s0, const uint8_t *s1, unsigned from,
                        unsigned pairs)
{
    unsigned x;

    for (x = from; x < pairs; x++) {
        y0[2 * x] = s0[4 * x];
        y0[2 * x + 1] = s0[4 * x + 2];
        y1[2 * x] = s1[4 * x];
        y1[2 * x + 1] = s1[4 * x + 2];
        u[x] = (uint8_t)((s0[4 * x + 1] + s1[4 * x + 1] + 1) >> 1);
        v[x] = (uint8_t)((s0[4 * x + 3] + s1[4 * x + 3] + 1) >> 1);
    }
}

#if defined(__ARM_NEON)
/* Sixteen pairs - 32 pixels, two rows - at a time. */
static unsigned pair_neon(uint8_t *y0, uint8_t *y1, uint8_t *u, uint8_t *v,
                          const uint8_t *s0, const uint8_t *s1,
                          unsigned pairs)
{
    unsigned x;

    for (x = 0; x + 16u <= pairs; x += 16u) {
        uint8x16x4_t a = vld4q_u8(s0 + 4u * x);      /* Y0 U Y1 V */
        uint8x16x4_t b = vld4q_u8(s1 + 4u * x);
        uint8x16x2_t ya = { { a.val[0], a.val[2] } };
        uint8x16x2_t yb = { { b.val[0], b.val[2] } };

        vst2q_u8(y0 + 2u * x, ya);
        vst2q_u8(y1 + 2u * x, yb);
        vst1q_u8(u + x, vrhaddq_u8(a.val[1], b.val[1]));
        vst1q_u8(v + x, vrhaddq_u8(a.val[3], b.val[3]));
    }

    return x;
}
#elif defined(__SSE2__)
/* Eight pairs - 16 pixels, two rows - at a time. */
static unsigned pair_sse2(uint8_t *y0, uint8_t *y1, uint8_t *u, uint8_t *v,
                          const uint8_t *s0, const uint8_t *s1,
                          unsigned pairs)
{
    const __m128i low = _mm_set1_epi16(0x00FF);
    unsigned x;

    for (x = 0; x + 8u <= pairs; x += 8u) {
        __m128i a0 = _mm_loadu_si128((const __m128i *)(const void *)(s0 + 4u * x));
        __m128i a1 = _mm_loadu_si128((const __m128i *)(const void *)(s0 + 4u * x + 16u));
        __m128i b0 = _mm_loadu_si128((const __m128i *)(const void *)(s1 + 4u * x));
        __m128i b1 = _mm_loadu_si128((const __m128i *)(const void *)(s1 + 4u * x + 16u));

        /* Y is every even byte; U and V the odd ones, U V U V. */
        __m128i ya = _mm_packus_epi16(_mm_and_si128(a0, low),
                                      _mm_and_si128(a1, low));
        __m128i yb = _mm_packus_epi16(_mm_and_si128(b0, low),
                                      _mm_and_si128(b1, low));
        __m128i ca = _mm_packus_epi16(_mm_srli_epi16(a0, 8),
                                      _mm_srli_epi16(a1, 8));
        __m128i cb = _mm_packus_epi16(_mm_srli_epi16(b0, 8),
                                      _mm_srli_epi16(b1, 8));
        __m128i c = _mm_avg_epu8(ca, cb);            /* U V U V ... */
        __m128i uu = _mm_packus_epi16(_mm_and_si128(c, low), _mm_setzero_si128());
        __m128i vv = _mm_packus_epi16(_mm_srli_epi16(c, 8), _mm_setzero_si128());

        _mm_storeu_si128((__m128i *)(void *)(y0 + 2u * x), ya);
        _mm_storeu_si128((__m128i *)(void *)(y1 + 2u * x), yb);
        _mm_storel_epi64((__m128i *)(void *)(u + x), uu);
        _mm_storel_epi64((__m128i *)(void *)(v + x), vv);
    }

    return x;
}
#endif

static void i420(uint8_t *y, uint8_t *u, uint8_t *v, unsigned y_stride,
                 unsigned uv_stride, const uint8_t *src, unsigned width,
                 unsigned height, bool vectors)
{
    unsigned pairs = width / 2u, row;

    for (row = 0; row < height; row += 2u) {
        const uint8_t *s0 = src + (size_t)row * width * 2u;
        const uint8_t *s1 = (row + 1u < height) ? s0 + (size_t)width * 2u : s0;
        uint8_t *y0 = y + (size_t)row * y_stride;
        uint8_t *y1 = (row + 1u < height) ? y0 + y_stride : y0;
        uint8_t *uo = u + (size_t)(row / 2u) * uv_stride;
        uint8_t *vo = v + (size_t)(row / 2u) * uv_stride;
        unsigned done = 0;

        if (vectors) {
#if defined(__ARM_NEON)
            done = pair_neon(y0, y1, uo, vo, s0, s1, pairs);
#elif defined(__SSE2__)
            done = pair_sse2(y0, y1, uo, vo, s0, s1, pairs);
#endif
        }

        pair_scalar(y0, y1, uo, vo, s0, s1, done, pairs);
    }
}

void gfx_yuy2_i420(uint8_t *y, uint8_t *u, uint8_t *v, unsigned y_stride,
                   unsigned uv_stride, const uint8_t *src, unsigned width,
                   unsigned height)
{
    i420(y, u, v, y_stride, uv_stride, src, width, height, true);
}

void gfx_yuy2_i420_scalar(uint8_t *y, uint8_t *u, uint8_t *v,
                          unsigned y_stride, unsigned uv_stride,
                          const uint8_t *src, unsigned width, unsigned height)
{
    i420(y, u, v, y_stride, uv_stride, src, width, height, false);
}
