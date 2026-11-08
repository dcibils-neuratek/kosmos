/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A row of quantised weights times the quantised input** - the loop the
 * LLM Kit spends its time in (`docs/xavier.md` X2). Each is ggml's
 * `vec_dot_q8_0_q8_0` and `vec_dot_q4_0_q8_0`: per 32-value block, an
 * integer dot of the two blocks' values, times the two blocks' F16 scales,
 * added to the row's sum in block order.
 *
 * Two of each: the vector one the kit uses, and the scalar one it is held
 * to by `tools/test_llmdot.c` - **to the bit**, because only the integer
 * work inside a block is in vectors, and an integer sum is the same in any
 * order. The float sum across blocks stays scalar and in order, so the two
 * cannot round differently.
 *
 * `w` is a row of `blocks` Q8_0 (34-byte) or Q4_0 (18-byte) blocks; `xq` is
 * the input as `blocks` Q8_0 blocks, and `xs` their scales as floats,
 * converted once for every row they meet. **The scales were the time, not
 * the arithmetic**: a block is two `sdot`s or four `pmaddwd`s, and a
 * conversion of its two F16 scales compiled branch-free was about fifty
 * instructions. The scalar references convert both scales themselves and
 * ignore `xs`, so the test holds `xs` to the conversion as well.
 */

#ifndef KOSMOS_LLM_DOT_H
#define KOSMOS_LLM_DOT_H

#include <stdint.h>

typedef float llm_dot_fn(const uint8_t *w, const uint8_t *xq, const float *xs, uint64_t blocks);

llm_dot_fn llm_dot_q8_0, llm_dot_q4_0;
llm_dot_fn llm_dot_q8_0_scalar, llm_dot_q4_0_scalar;

/*
 * **One row against four inputs at once** (`docs/xavier.md`, *Making it
 * fast*): a block of weights is unpacked once - Q4_0's nibbles made bytes,
 * the scale converted - and met by four inputs, where four calls of the
 * one-input loop did that four times. Input `t` is at `xq + t * xq_room`
 * and its scales at `xs + t * xs_room`; its answer in `out[t]`. Each
 * answer is the one-input loop's to the bit: the integer work in a block is
 * exact in any order, and each input's float sum still runs in block order.
 */
#define LLM_DOT4 4

typedef void llm_dot4_fn(const uint8_t *w, const uint8_t *xq, uint64_t xq_room, const float *xs,
                         uint64_t xs_room, uint64_t blocks, float out[LLM_DOT4]);

llm_dot4_fn llm_dot4_q8_0, llm_dot4_q4_0;

/*
 * **The same four loops, 32 bytes wide, for AVX2** (`llm_dot_avx2.c`,
 * x86-64 only), and whether this processor and this system can run them:
 * AVX2 in CPUID and the upper halves kept, which XGETBV says.
 */
#if defined(__x86_64__)
#include <stdbool.h>

llm_dot_fn llm_dot_q8_0_avx2, llm_dot_q4_0_avx2;
llm_dot4_fn llm_dot4_q8_0_avx2, llm_dot4_q4_0_avx2;
bool llm_avx2_present(void);
#endif

#endif
