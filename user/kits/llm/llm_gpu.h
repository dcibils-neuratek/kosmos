/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The LLM Kit's products on the GPU** (`docs/xavier.md`, *X7, the GPU:
 * the design*). What a product needs from a GPU is three things - a kernel
 * handed over, buffers both sides see, and a run waited for - and those are
 * a device's, given
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
 *     llm_offload(&model, llm_gpu_product, llm_gpu_done, &g);
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

  /* A buffer of at least `bytes` both sides see: its id and the
   * processor's address of it. `read_back` when the processor reads what
   * the GPU wrote - the sums - and so wants it in its caches rather than
   * write-combining, which it reads a word at a time (18.729). */
  const char *(*buffer)(void *ctx, size_t bytes, int read_back, uint32_t *id, void **cpu);

  /* A buffer from `buffer` let go: a larger one replaces it, or the
   * model is unloaded. */
  void (*forget)(void *ctx, uint32_t id);

  /* A run begun, and not waited for: `groups` thread groups of `simd`
   * lanes, the buffers by id in the kernel's order, and its constants. */
  const char *(*run)(void *ctx, uint32_t kernel, uint32_t simd, uint32_t groups,
                     const uint32_t *buffers, uint32_t nbuffers, const void *constants,
                     size_t nconstants);

  /* The run begun waited for; `*already` whether it had finished before
   * this was asked - which is how the rows are shared out (G5). */
  const char *(*wait)(void *ctx, int *already);
};

#define LLM_GPU_TENSORS 512u     /* weights laid out for it, one buffer each */
#define LLM_GPU_SHARE_STEP 8u    /* of 1024, a product's change of share */

struct llm_gpu {
  struct llm_gpu_device dev;
  uint32_t kernel, simd;

  /* Each weight it has taken, copied once into a buffer of its own in the
   * GPU's layout (`llm_gpu.c`, `weight`): the copy is the price of reading
   * at a line a read, and it is made only for what the GPU is given. */
  struct { const void *at; uint32_t id; size_t bytes; } tensor[LLM_GPU_TENSORS];
  uint32_t ntensors;
  uint64_t tiled_bytes;

  /* The inputs' values, a record a pair of blocks of their scales and
   * sums, and the products' sums, grown as needed. */
  uint32_t xq_id, xm_id, out_id;
  void *xq, *xm, *out;
  size_t xq_bytes, xm_bytes, out_bytes;

  /* The product begun and not yet finished (`llm_gpu_done`). */
  struct { int on; float *out; size_t out_room; const float *bias; uint32_t n, rows; } pending;

  /* **How much of a product the GPU begins** (G5), out of 1024: a
   * question's and an answer's apart, since the GPU and the processor
   * differ in each. A step up when the GPU was done before the processor's
   * rows were, a step down when it was still running - so the two finish
   * together, and nothing here needs a clock. */
  uint32_t share[2];

  uint64_t products;             /* taken here, whole or in part */
  uint64_t rows_gpu, rows_all;   /* the rows begun here, of every product's offered */
  uint64_t declined;             /* offered and left to the processor */
  char why[160];                 /* why the last one was left, if it was */
};

/* The kernel handed to `dev`: nought, or less and why. */
int llm_gpu_begin(struct llm_gpu *g, const struct llm_gpu_device *dev, char *why, size_t room);

/* Every weight's buffer let go - a model unloaded - and the inputs' and
 * sums' kept. The kernel stays: it is the same for the next model. */
void llm_gpu_forget(struct llm_gpu *g);

/* `llm_product_fn` and `llm_product_done_fn`, for `llm_offload` with a
 * `struct llm_gpu`. Declines, and says why in `g->why`, what the kernel
 * does not take: a weight not in Q4_0, an odd number of blocks to a row, or
 * a device that refused. */
llm_product_fn llm_gpu_product;
llm_product_done_fn llm_gpu_done;

#endif
