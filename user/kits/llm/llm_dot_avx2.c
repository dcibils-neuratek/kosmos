/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The LLM Kit's dot products, 32 bytes wide, for a processor with AVX2**
 * (`docs/xavier.md`, *Making it fast*, X2b). The same loops as
 * `llm_dot.c`, in the same GCC vector types, compiled a second time with
 * `target("avx2")` - and chosen at `llm_open` only where CPUID says AVX2
 * and XGETBV says the system keeps the upper halves (`llm_avx2_present`).
 * Nebula keeps them since 0.11.325: its switch is XSAVE with AVX in XCR0
 * where the processor has it.
 *
 * A block is 32 values, so a whole block is one register here where SSE2
 * took two. Each byte widens to 16 bits and `pmaddwd` multiplies and adds
 * neighbours into 32-bit lanes: at most 2 x 128 x 128, exact. `pmaddubsw`
 * would skip the widening and saturates when both values are -128, which
 * the test's corners are, so it is not used.
 *
 * **To the bit, as `llm_dot.c` is**: the integer work in a block is exact
 * in any order, and the float sum across blocks stays scalar and in block
 * order. FMA is left out of the target on purpose - with it the compiler
 * may fuse `sum + s * scale` into one rounding where the reference takes
 * two.
 *
 * Only on x86-64; on ARM the file is empty and `llm_avx2_present` is not
 * asked.
 */

#include "llm_dot.h"
#include "llm_core.h"

#if defined(__x86_64__)

#define AVX2 __attribute__((target("avx2")))

typedef int8_t   i8x16  __attribute__((vector_size(16)));
typedef uint8_t  u8x16  __attribute__((vector_size(16)));
typedef int16_t  i16x16 __attribute__((vector_size(32)));
typedef int32_t  i32x8  __attribute__((vector_size(32)));

static inline float scale(const uint8_t *b)
{
  return llm_f16_to_f32((uint16_t)(b[0] | b[1] << 8));
}

/* Sixteen bytes, signed, as sixteen shorts: `vpmovsxbw`. */
static inline AVX2 i16x16 widen(const uint8_t *p)
{
  i8x16 v;
  __builtin_memcpy(&v, p, 16);
  return __builtin_convertvector(v, i16x16);
}

/* Sixteen shorts times sixteen, neighbours added into eight ints. */
static inline AVX2 i32x8 madd(i16x16 a, i16x16 b)
{
  return (i32x8)__builtin_ia32_pmaddwd256(a, b);
}

static inline AVX2 int32_t lanes(i32x8 s)
{
  return s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + s[7];
}

/* A Q4_0 block's 32 values, less eight: low nibbles first, then high. */
static inline AVX2 void nibbles(const uint8_t *w, i16x16 *lo, i16x16 *hi)
{
  const u8x16 low = (u8x16){0} + 0x0f, eight = (u8x16){0} + 8;
  u8x16 q;
  __builtin_memcpy(&q, w + 2, 16);
  *lo = __builtin_convertvector((i8x16)((q & low) - eight), i16x16);
  *hi = __builtin_convertvector((i8x16)((q >> 4) - eight), i16x16);
}

AVX2 float llm_dot_q8_0_avx2(const uint8_t *w, const uint8_t *xq, const float *xs, uint64_t blocks)
{
  float sum = 0;

  for (uint64_t b = 0; b < blocks; b++, w += 34, xq += 34) {
    int32_t s = lanes(madd(widen(w + 2), widen(xq + 2)) + madd(widen(w + 18), widen(xq + 18)));
    sum += (float)s * (scale(w) * xs[b]);
  }
  return sum;
}

AVX2 float llm_dot_q4_0_avx2(const uint8_t *w, const uint8_t *xq, const float *xs, uint64_t blocks)
{
  float sum = 0;

  for (uint64_t b = 0; b < blocks; b++, w += 18, xq += 34) {
    i16x16 lo, hi;
    nibbles(w, &lo, &hi);
    int32_t s = lanes(madd(lo, widen(xq + 2)) + madd(hi, widen(xq + 18)));
    sum += (float)s * (scale(w) * xs[b]);
  }
  return sum;
}

/* Four inputs to a row: the block's weights widened once, met by four. */

AVX2 void llm_dot4_q8_0_avx2(const uint8_t *w, const uint8_t *xq, uint64_t xq_room, const float *xs,
                             uint64_t xs_room, uint64_t blocks, float out[LLM_DOT4])
{
  float sum[LLM_DOT4] = { 0 };

  for (uint64_t b = 0; b < blocks; b++, w += 34) {
    const i16x16 lo = widen(w + 2), hi = widen(w + 18);
    const float sw = scale(w);

    for (int t = 0; t < LLM_DOT4; t++) {
      const uint8_t *x = xq + t * xq_room + b * 34;
      int32_t s = lanes(madd(lo, widen(x + 2)) + madd(hi, widen(x + 18)));
      sum[t] += (float)s * (sw * xs[t * xs_room + b]);
    }
  }

  for (int t = 0; t < LLM_DOT4; t++) out[t] = sum[t];
}

AVX2 void llm_dot4_q4_0_avx2(const uint8_t *w, const uint8_t *xq, uint64_t xq_room, const float *xs,
                             uint64_t xs_room, uint64_t blocks, float out[LLM_DOT4])
{
  float sum[LLM_DOT4] = { 0 };

  for (uint64_t b = 0; b < blocks; b++, w += 18) {
    i16x16 lo, hi;
    nibbles(w, &lo, &hi);
    const float sw = scale(w);

    for (int t = 0; t < LLM_DOT4; t++) {
      const uint8_t *x = xq + t * xq_room + b * 34;
      int32_t s = lanes(madd(lo, widen(x + 2)) + madd(hi, widen(x + 18)));
      sum[t] += (float)s * (sw * xs[t * xs_room + b]);
    }
  }

  for (int t = 0; t < LLM_DOT4; t++) out[t] = sum[t];
}

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
  __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

/*
 * AVX2 is usable when the processor has it (leaf 7, EBX bit 5), has AVX
 * (leaf 1, ECX bit 28), and the system saves what it touches: OSXSAVE
 * (leaf 1, ECX bit 27) and XCR0's SSE and AVX bits both set. A processor
 * with AVX2 under a system that does not keep the upper halves would give
 * one thread another's registers, so all four are asked, every one.
 */
bool llm_avx2_present(void)
{
  uint32_t r[4];

  cpuid(0, 0, r);
  if (r[0] < 7) return false;

  cpuid(1, 0, r);
  if (!(r[2] & (1u << 27)) || !(r[2] & (1u << 28))) return false;

  uint32_t lo, hi;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  if ((lo & 0x6u) != 0x6u) return false;

  cpuid(7, 0, r);
  return (r[1] & (1u << 5)) != 0;
}

#endif
