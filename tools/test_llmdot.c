/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The LLM Kit's vector dot products held to its scalar ones, to the bit**
 * (`docs/xavier.md` X2). Built twice, as `test_yuv` is: natively, where the
 * lanes are NEON, and as `test_llmdot_x86`, which Rosetta runs and where
 * they are SSE2.
 *
 * Rows of every length from 1 to 96 blocks and phi-2's own two (80 blocks
 * for 2560 inputs, 320 for 10240), of random bytes and of the corners - all
 * -128, all 127, alternating - with random F16 scales, zero among them.
 * Each row's answer from the vector loop must be the scalar loop's exact
 * bits. Then the two timed on phi-2's widest product, which is what the
 * Mac's core says and not what Kosmos's will.
 */

#include "llm_core.h"
#include "llm_dot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int checks, failed;
static uint32_t seed = 12345;

static uint32_t rnd(void)
{
  seed = seed * 1664525u + 1013904223u;
  return seed >> 8;
}

static double now(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

/* A plausible F16 scale: 2^-14 to about 2^-2, either sign, sometimes 0. */
static void put_scale(uint8_t *b)
{
  uint16_t h = (uint16_t)(rnd() % 7 == 0 ? 0 : ((rnd() & 1) << 15) | ((1 + rnd() % 13) << 10) | (rnd() & 0x3ff));
  b[0] = (uint8_t)h;
  b[1] = (uint8_t)(h >> 8);
}

/* Values for a row: random, or one of the corners. */
static uint8_t value(int how, int i)
{
  switch (how) {
    case 1: return 0x80;                          /* -128 as Q8_0, 8 and 8 as Q4_0 */
    case 2: return 0x7f;
    case 3: return i & 1 ? 0x80 : 0x7f;
    case 4: return 0xff;
    default: return (uint8_t)rnd();
  }
}

static void fill(uint8_t *row, uint64_t blocks, int bytes, int how)
{
  for (uint64_t b = 0; b < blocks; b++) {
    put_scale(row + b * bytes);
    for (int i = 2; i < bytes; i++) row[b * bytes + i] = value(how, i);
  }
}

static int same(float a, float b)
{
  return memcmp(&a, &b, sizeof a) == 0;
}

/* Each set of loops the kit can choose, and whether this machine runs it. */
struct loops {
  const char *name;
  llm_dot_fn *q8, *q4;
  llm_dot4_fn *four8, *four4;
  llm_dot_k_fn *q6k;
};

/* A Q6_K row (210 bytes a block: 208 of values and scales, an F16 scale
 * last) and a Q8_K input (a float scale, 256 bytes). */
static void fill_q6k(uint8_t *row, uint64_t blocks, int how)
{
  for (uint64_t b = 0; b < blocks; b++) {
    for (int i = 0; i < 208; i++) row[b * 210 + i] = value(how, i);
    put_scale(row + b * 210 + 208);
  }
}

static void fill_q8k(uint8_t *x, uint64_t blocks, int how)
{
  for (uint64_t b = 0; b < blocks; b++) {
    float d = llm_f16_to_f32((uint16_t)(rnd() & 0x7bff)) * (rnd() & 1 ? -1 : 1);
    memcpy(x + b * 260, &d, 4);
    for (int i = 0; i < 256; i++) x[b * 260 + 4 + i] = value(how, i);
  }
}

int main(void)
{
  struct loops sets[2] = { { "SSE2/NEON", llm_dot_q8_0, llm_dot_q4_0, llm_dot4_q8_0, llm_dot4_q4_0, llm_dot_q6_k } };
  int nsets = 1;

#if defined(__x86_64__)
  if (llm_avx2_present()) {
    sets[nsets++] = (struct loops){ "AVX2", llm_dot_q8_0_avx2, llm_dot_q4_0_avx2,
                                    llm_dot4_q8_0_avx2, llm_dot4_q4_0_avx2,
                                    llm_dot_q6_k_avx2 };
  } else {
    printf("  (no AVX2 here: its loops are not asked - the x86 guest suites are)\n");
  }
#endif

  static uint8_t w8[320 * 34], w4[320 * 18], xq[320 * 34];
  uint64_t lengths[100];
  int n = 0;

  for (uint64_t i = 1; i <= 96; i++) lengths[n++] = i;
  lengths[n++] = 80;
  lengths[n++] = 320;

  int mism8 = 0, mism4 = 0;
  static float xs[320];

  for (int set = 0; set < nsets; set++)
  for (int l = 0; l < n; l++) {
    const struct loops *L = &sets[set];
    for (int how = 0; how < 5; how++) {
      for (int again = 0; again < (how ? 1 : 8); again++) {
        uint64_t blocks = lengths[l];
        fill(w8, blocks, 34, how);
        fill(w4, blocks, 18, how);
        fill(xq, blocks, 34, (how + again) % 5);
        for (uint64_t b = 0; b < blocks; b++) xs[b] = llm_f16_to_f32((uint16_t)(xq[b * 34] | xq[b * 34 + 1] << 8));

        float v8 = L->q8(w8, xq, xs, blocks), s8 = llm_dot_q8_0_scalar(w8, xq, xs, blocks);
        float v4 = L->q4(w4, xq, xs, blocks), s4 = llm_dot_q4_0_scalar(w4, xq, xs, blocks);
        checks += 2;

        /* Four inputs against the row at once: each the scalar's bits
         * for its own input. Four different inputs, so one answer put
         * in another's place cannot pass. */
        {
          static uint8_t x4[LLM_DOT4][320 * 34];
          static float s4x[LLM_DOT4][320];
          float o8[LLM_DOT4], o4[LLM_DOT4];

          for (int t = 0; t < LLM_DOT4; t++) {
            fill(x4[t], blocks, 34, (how + again + t + 1) % 5);
            for (uint64_t b = 0; b < blocks; b++)
              s4x[t][b] = llm_f16_to_f32((uint16_t)(x4[t][b * 34] | x4[t][b * 34 + 1] << 8));
          }

          L->four8(w8, x4[0], sizeof x4[0], s4x[0], 320, blocks, o8);
          L->four4(w4, x4[0], sizeof x4[0], s4x[0], 320, blocks, o4);

          for (int t = 0; t < LLM_DOT4; t++) {
            checks += 2;
            if (!same(o8[t], llm_dot_q8_0_scalar(w8, x4[t], s4x[t], blocks))
                || !same(o4[t], llm_dot_q4_0_scalar(w4, x4[t], s4x[t], blocks))) {
              if (failed < 5) printf("  FAIL: %s, four inputs at once, %llu blocks, kind %d, input %d: "
                                     "not the scalar's bits\n", L->name, (unsigned long long)blocks, how, t);
              failed++;
            }
          }
        }

        /* Q6_K, its blocks of 256 for each length of 32-value ones. */
        if (blocks <= 24) {
          static uint8_t w6[24 * 210], xk[24 * 260];
          fill_q6k(w6, blocks, how);
          fill_q8k(xk, blocks, (how + again) % 5);
          float v6 = L->q6k(w6, xk, blocks), s6 = llm_dot_q6_k(w6, xk, blocks);
          checks++;
          if (!same(v6, s6)) {
            if (failed < 5) printf("  FAIL: %s Q6_K, %llu blocks, kind %d: %.9g against the plain loop %.9g\n",
                                   L->name, (unsigned long long)blocks, how, v6, s6);
            failed++;
          }
        }

        if (!same(v8, s8)) {
          if (mism8++ < 5) printf("  FAIL: %s Q8_0, %llu blocks, kind %d: %.9g against the scalar %.9g\n",
                                  L->name, (unsigned long long)blocks, how, v8, s8);
          failed++;
        }
        if (!same(v4, s4)) {
          if (mism4++ < 5) printf("  FAIL: %s Q4_0, %llu blocks, kind %d: %.9g against the scalar %.9g\n",
                                  L->name, (unsigned long long)blocks, how, v4, s4);
          failed++;
        }
      }
    }
  }

  /* The speed of each, on a 10240-wide row - phi-2's MLP down - many
   * times over, so the numbers are the loops' and not the clock's. */
  fill(w8, 320, 34, 0);
  fill(w4, 320, 18, 0);
  fill(xq, 320, 34, 0);
  for (uint64_t b = 0; b < 320; b++) xs[b] = llm_f16_to_f32((uint16_t)(xq[b * 34] | xq[b * 34 + 1] << 8));

  const int reps = 20000;
  volatile float sink = 0;
  double t[4];
  llm_dot_fn *const fn[4] = {
      llm_dot_q8_0_scalar, llm_dot_q8_0, llm_dot_q4_0_scalar, llm_dot_q4_0};

  for (int f = 0; f < 4; f++) {
    const uint8_t *w = f < 2 ? w8 : w4;
    double t0 = now();
    for (int r = 0; r < reps; r++) sink += fn[f](w, xq, xs, 320);
    t[f] = (now() - t0) / reps * 1e9 / (320 * 32);
  }
  (void)sink;

  printf("a value: Q8_0 %.3f ns scalar, %.3f in vectors (%.1fx); Q4_0 %.3f scalar, %.3f in vectors "
         "(%.1fx) - this Mac's core, not Kosmos's\n",
         t[0], t[1], t[0] / t[1], t[2], t[3], t[2] / t[3]);

  if (nsets > 1) {
    double ta[2];
    llm_dot_fn *const fa[2] = { sets[1].q8, sets[1].q4 };

    for (int f = 0; f < 2; f++) {
      const uint8_t *w = f == 0 ? w8 : w4;
      double t0 = now();
      for (int r = 0; r < reps; r++) sink += fa[f](w, xq, xs, 320);
      ta[f] = (now() - t0) / reps * 1e9 / (320 * 32);
    }
    printf("a value in AVX2: Q8_0 %.3f ns (%.1fx the scalar), Q4_0 %.3f (%.1fx)\n",
           ta[0], t[0] / ta[0], ta[1], t[2] / ta[1]);

    /* Q6_K on Qwen2.5 1.5B's output row, 1536 wide. */
    static uint8_t w6[6 * 210], xk[6 * 260];
    double tk[2];
    llm_dot_k_fn *const fk[2] = { llm_dot_q6_k, sets[1].q6k };
    fill_q6k(w6, 6, 0);
    fill_q8k(xk, 6, 0);
    for (int f = 0; f < 2; f++) {
      double t0 = now();
      for (int r = 0; r < reps * 20; r++) sink += fk[f](w6, xk, 6);
      tk[f] = (now() - t0) / (reps * 20) * 1e9 / 1536;
    }
    printf("a value of Q6_K: %.3f ns plain, %.3f in AVX2 (%.1fx)\n", tk[0], tk[1], tk[0] / tk[1]);
  }

  if (failed) {
    printf("FAIL: %d of %d rows where the vector dot product is not the scalar one's bits\n", failed,
           checks);
    return 1;
  }
  printf("PASS: %d rows, Q8_0, Q4_0 and Q6_K, the vector dot product and four inputs at once the scalar "
         "one's bits every time (1 to 96 blocks, phi-2's 80 and 320, random and the corners; %s)\n",
         checks, nsets > 1 ? "SSE2 and AVX2" : sets[0].name);
  return 0;
}
