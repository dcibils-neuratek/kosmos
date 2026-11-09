/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The LLM Kit's products on the GPU** (`docs/xavier.md`, *X7, the GPU:
 * the design*). What a product needs from a GPU is four things - a kernel
 * handed over, a stretch of the model's memory the GPU may read, a buffer
 * both sides see, and a run waited for - and those are a device's, given
 * here as functions: on Kosmos the GPU's driver behind `/Devices/gpu`
 * (the LLM server's), and on the Mac the emulator of the execution units
 * (`tools/test_llm.c --gpu`). Everything else - which products go, the
 * inputs laid out, the constants, the answers read back with the bias -
 * is here, once, for both.
 *
 * The kernel is `user/lib/llmgpu.lua`'s `q4_0`, compiled by `kc.lua` when
 * the kit is built (`llmgpu_kernels.h`), and its sums are the processor's
 * to the bit: what the GPU answers is what `llm_dot_q4_0` would.
 *
 *   static struct llm_gpu g;
 *   if (llm_gpu_begin(&g, &device, why, sizeof why) == 0)
 *     llm_offload(&model, llm_gpu_product, &g);
 */

#ifndef KOSMOS_LLM_GPU_H
#define KOSMOS_LLM_GPU_H

#include <stddef.h>
#include <stdint.h>

#include "llm_core.h"

/* Each answers NULL, or why not - a sentence for a person. */
struct llm_gpu_device {
  void *ctx;

  /* The kernel's bytes handed over: its id. */
  const char *(*kernel)(void *ctx, const uint8_t *code, size_t bytes, uint32_t *id);

  /* `bytes` of the model's memory from `at`, for the GPU to read where
   * they are: a buffer's id, and where in it `at` is. */
  const char *(*import)(void *ctx, const void *at, size_t bytes, uint32_t *id, uint32_t *offset);

  /* A buffer of at least `bytes` both sides see: its id and the
   * processor's address of it. */
  const char *(*buffer)(void *ctx, size_t bytes, uint32_t *id, void **cpu);

  /* A buffer from `buffer` let go, when a larger one replaces it. */
  void (*forget)(void *ctx, uint32_t id);

  /* A run, done before it answers: `groups` thread groups of `simd`
   * lanes, the buffers by id in the kernel's order, and its constants. */
  const char *(*run)(void *ctx, uint32_t kernel, uint32_t simd, uint32_t groups,
                     const uint32_t *buffers, uint32_t nbuffers, const void *constants,
                     size_t nconstants);
};

#define LLM_GPU_TENSORS 512u     /* weights imported, one buffer each */

struct llm_gpu {
  struct llm_gpu_device dev;
  uint32_t kernel, simd;

  struct { const void *at; uint32_t id, offset; } tensor[LLM_GPU_TENSORS];
  uint32_t ntensors;

  /* The inputs' values, their scales and the sums, grown as needed. */
  uint32_t xq_id, xs_id, out_id;
  void *xq, *xs, *out;
  size_t xq_bytes, xs_bytes, out_bytes;

  uint64_t products;             /* taken here */
  uint64_t declined;             /* offered and left to the processor */
  char why[160];                 /* why the last one was left, if it was */
};

/* The kernel handed to `dev`: nought, or less and why. */
int llm_gpu_begin(struct llm_gpu *g, const struct llm_gpu_device *dev, char *why, size_t room);

/* Every weight's buffer let go - a model unloaded - and the inputs' and
 * sums' kept. The kernel stays: it is the same for the next model. */
void llm_gpu_forget(struct llm_gpu *g);

/* `llm_product_fn`, for `llm_offload` with a `struct llm_gpu`. Declines,
 * and says why in `g->why`, what the kernel does not take: a weight not in
 * Q4_0, an odd number of blocks to a row, or a device that refused. */
llm_product_fn llm_gpu_product;

#endif
