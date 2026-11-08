/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The LLM Kit's dot products, in GCC's vector types: one source that the
 * compiler makes NEON on ARM and SSE2 on a PC (`llm_dot.h`).
 *
 * The lanes load through `__builtin_memcpy` of a vector's size, which is
 * one instruction; plain `memcpy` under `-ffreestanding` is a call into the
 * libc's loop (`user/kits/gfx/pack.c`).
 *
 * A block is 32 signed bytes against 32 signed bytes. Each byte widens to
 * 16 bits and the products are taken there - at most 128 x 128 = 16384,
 * which fits - then neighbouring products are added into 32-bit lanes,
 * which is SSE2's `pmaddwd` and NEON's `smull` + `sadalp` in shape, and the
 * four lanes are summed. Every step is integer, so the result is the
 * scalar loop's exactly. Sixteen bytes wide, SSE2's width: a wider vector
 * without AVX is two registers and, crossing a function, a different ABI.
 */

#include "llm_dot.h"
#include "llm_core.h"

typedef int8_t   i8x8   __attribute__((vector_size(8)));
typedef int8_t   i8x16  __attribute__((vector_size(16)));
typedef uint8_t  u8x16  __attribute__((vector_size(16)));
typedef int16_t  i16x4  __attribute__((vector_size(8)));
typedef int16_t  i16x8  __attribute__((vector_size(16)));
typedef int32_t  i32x4  __attribute__((vector_size(16)));

static inline float scale(const uint8_t *b)
{
  return llm_f16_to_f32((uint16_t)(b[0] | b[1] << 8));
}

/* Eight shorts times eight, neighbours added into four ints: `pmaddwd`. */
static inline i32x4 madd(i16x8 a, i16x8 b)
{
  i16x8 p = a * b;
  i16x4 even = __builtin_shufflevector(p, p, 0, 2, 4, 6);
  i16x4 odd = __builtin_shufflevector(p, p, 1, 3, 5, 7);
  return __builtin_convertvector(even, i32x4) + __builtin_convertvector(odd, i32x4);
}

/* Sixteen bytes against sixteen, into four 32-bit lanes. */
static inline i32x4 dot16(i8x16 a, i8x16 b)
{
  i8x8 al = __builtin_shufflevector(a, a, 0, 1, 2, 3, 4, 5, 6, 7);
  i8x8 ah = __builtin_shufflevector(a, a, 8, 9, 10, 11, 12, 13, 14, 15);
  i8x8 bl = __builtin_shufflevector(b, b, 0, 1, 2, 3, 4, 5, 6, 7);
  i8x8 bh = __builtin_shufflevector(b, b, 8, 9, 10, 11, 12, 13, 14, 15);
  return madd(__builtin_convertvector(al, i16x8), __builtin_convertvector(bl, i16x8))
       + madd(__builtin_convertvector(ah, i16x8), __builtin_convertvector(bh, i16x8));
}

static inline int32_t lanes(i32x4 s)
{
  return s[0] + s[1] + s[2] + s[3];
}

float llm_dot_q8_0(const uint8_t *w, const uint8_t *xq, const float *xs, uint64_t blocks)
{
  float sum = 0;

  for (uint64_t b = 0; b < blocks; b++, w += 34, xq += 34) {
    i8x16 a0, a1, x0, x1;
    __builtin_memcpy(&a0, w + 2, 16);
    __builtin_memcpy(&a1, w + 18, 16);
    __builtin_memcpy(&x0, xq + 2, 16);
    __builtin_memcpy(&x1, xq + 18, 16);
    int32_t s = lanes(dot16(a0, x0) + dot16(a1, x1));
    sum += (float)s * (scale(w) * xs[b]);
  }
  return sum;
}

float llm_dot_q4_0(const uint8_t *w, const uint8_t *xq, const float *xs, uint64_t blocks)
{
  float sum = 0;
  const u8x16 low = (u8x16){0} + 0x0f, eight = (u8x16){0} + 8;

  for (uint64_t b = 0; b < blocks; b++, w += 18, xq += 34) {
    u8x16 q;
    i8x16 x0, x1;
    __builtin_memcpy(&q, w + 2, 16);
    __builtin_memcpy(&x0, xq + 2, 16);
    __builtin_memcpy(&x1, xq + 18, 16);
    /* Low nibbles are values 0-15, high ones 16-31; each less eight. */
    i8x16 lo = (i8x16)((q & low) - eight), hi = (i8x16)((q >> 4) - eight);
    int32_t s = lanes(dot16(lo, x0) + dot16(hi, x1));
    sum += (float)s * (scale(w) * xs[b]);
  }
  return sum;
}

/* The references, as X1 wrote them. */

float llm_dot_q8_0_scalar(const uint8_t *w, const uint8_t *xq, const float *xs, uint64_t blocks)
{
  (void)xs;
  float sum = 0;

  for (uint64_t b = 0; b < blocks; b++, w += 34, xq += 34) {
    const int8_t *qa = (const int8_t *)(w + 2), *qb = (const int8_t *)(xq + 2);
    int32_t s = 0;
    for (int i = 0; i < LLM_QK; i++) s += qa[i] * qb[i];
    sum += (float)s * (scale(w) * scale(xq));
  }
  return sum;
}

float llm_dot_q4_0_scalar(const uint8_t *w, const uint8_t *xq, const float *xs, uint64_t blocks)
{
  (void)xs;
  float sum = 0;

  for (uint64_t b = 0; b < blocks; b++, w += 18, xq += 34) {
    const uint8_t *qa = w + 2;
    const int8_t *qb = (const int8_t *)(xq + 2);
    int32_t s = 0;
    for (int i = 0; i < LLM_QK / 2; i++) {
      s += ((qa[i] & 0x0f) - 8) * qb[i];
      s += ((qa[i] >> 4) - 8) * qb[i + LLM_QK / 2];
    }
    sum += (float)s * (scale(w) * scale(xq));
  }
  return sum;
}
