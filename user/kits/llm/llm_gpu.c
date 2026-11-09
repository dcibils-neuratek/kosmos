/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The LLM Kit's products on the GPU (`llm_gpu.h`, `docs/xavier.md` X7).
 */

#include <string.h>

#include "llm_gpu.h"
#include "llmgpu_kernels.h"      /* made from user/lib/llmgpu.lua when the kit is built */

static int declined(struct llm_gpu *g, const char *why)
{
  size_t n = strlen(why);

  if (n >= sizeof g->why) n = sizeof g->why - 1;
  memcpy(g->why, why, n);
  g->why[n] = '\0';
  g->declined++;
  return -1;
}

int llm_gpu_begin(struct llm_gpu *g, const struct llm_gpu_device *dev, char *why, size_t room)
{
  const char *no;

  memset(g, 0, sizeof *g);
  g->dev = *dev;
  g->simd = LLMGPU_Q4_0_SIMD;

  if ((no = dev->kernel(dev->ctx, llmgpu_q4_0, sizeof llmgpu_q4_0, &g->kernel)) != NULL) {
    size_t n = strlen(no);

    if (room > 0) {
      if (n >= room) n = room - 1;
      memcpy(why, no, n);
      why[n] = '\0';
    }

    return -1;
  }

  return 0;
}

void llm_gpu_forget(struct llm_gpu *g)
{
  for (uint32_t i = 0; i < g->ntensors; i++) g->dev.forget(g->dev.ctx, g->tensor[i].id);

  g->ntensors = 0;
}

/* A buffer of the device's of at least `bytes`, grown when it is not. */
static const char *room_for(struct llm_gpu *g, size_t bytes, uint32_t *id, void **cpu, size_t *have)
{
  const char *no;
  size_t want = *have ? *have : 64u * 1024u;

  if (bytes <= *have) return NULL;

  while (want < bytes) want *= 2;

  if (*have) g->dev.forget(g->dev.ctx, *id);

  *have = 0;

  if ((no = g->dev.buffer(g->dev.ctx, want, id, cpu)) != NULL) return no;

  *have = want;
  return NULL;
}

/* The weight's buffer: imported the first time it is used, and kept. */
static const char *weight(struct llm_gpu *g, const struct llm_tensor *w, uint32_t *id, uint32_t *offset)
{
  const char *no;

  for (uint32_t i = 0; i < g->ntensors; i++) {
    if (g->tensor[i].at == w->data) {
      *id = g->tensor[i].id;
      *offset = g->tensor[i].offset;
      return NULL;
    }
  }

  if (g->ntensors == LLM_GPU_TENSORS) return "more weights than the GPU path keeps";

  if ((no = g->dev.import(g->dev.ctx, w->data, (size_t)w->bytes, id, offset)) != NULL) return no;

  g->tensor[g->ntensors].at = w->data;
  g->tensor[g->ntensors].id = *id;
  g->tensor[g->ntensors].offset = *offset;
  g->ntensors++;
  return NULL;
}

int llm_gpu_product(void *ctx, const struct llm_tensor *w, const uint8_t *xq, size_t xq_room,
                    const float *xs, size_t xs_room, uint32_t n, const float *bias,
                    float *out, size_t out_room)
{
  struct llm_gpu *g = ctx;
  uint64_t cols = w->ne[0], rows = w->ne[1], blocks = cols / LLM_QK;
  uint32_t wid, w_at, ids[4];
  const char *no;

  if (w->type != LLM_Q4_0) return declined(g, "not a Q4_0 weight");
  if (blocks == 0 || blocks % 2 != 0) return declined(g, "an odd number of blocks to a row");
  if (rows == 0 || rows > 0x7fffffffu / 4u || n == 0) return declined(g, "no rows");

  if ((no = weight(g, w, &wid, &w_at)) != NULL
      || (no = room_for(g, (size_t)n * blocks * 32u, &g->xq_id, &g->xq, &g->xq_bytes)) != NULL
      || (no = room_for(g, (size_t)n * blocks * 4u, &g->xs_id, &g->xs, &g->xs_bytes)) != NULL
      || (no = room_for(g, (size_t)n * rows * 4u, &g->out_id, &g->out, &g->out_bytes)) != NULL)
    return declined(g, no);

  /* The inputs as the kernel reads them: Q8_0's blocks less their scales,
   * so every value is in whole dwords, and the scales as floats. */
  for (uint32_t t = 0; t < n; t++) {
    uint8_t *to = (uint8_t *)g->xq + (size_t)t * blocks * 32u;
    const uint8_t *from = xq + t * xq_room;

    for (uint64_t b = 0; b < blocks; b++) memcpy(to + b * 32u, from + b * 34u + 2u, 32u);
    memcpy((float *)g->xs + (size_t)t * blocks, xs + t * xs_room, (size_t)blocks * sizeof(float));
  }

  int32_t constants[4];
  constants[LLMGPU_Q4_0_ROWS] = (int32_t)rows;
  constants[LLMGPU_Q4_0_BLOCKS] = (int32_t)blocks;
  constants[LLMGPU_Q4_0_W_AT] = (int32_t)w_at;
  constants[LLMGPU_Q4_0_INPUTS] = (int32_t)n;

  ids[LLMGPU_Q4_0_W] = wid;
  ids[LLMGPU_Q4_0_XQ] = g->xq_id;
  ids[LLMGPU_Q4_0_XS] = g->xs_id;
  ids[LLMGPU_Q4_0_OUT] = g->out_id;

  if ((no = g->dev.run(g->dev.ctx, g->kernel, g->simd, (uint32_t)((rows + g->simd - 1) / g->simd), ids, 4,
                       constants, sizeof constants)) != NULL)
    return declined(g, no);

  /* The sums, and the bias added as the processor adds it. */
  const float *sums = g->out;

  for (uint32_t t = 0; t < n; t++)
    for (uint64_t r = 0; r < rows; r++) {
      float v = sums[(size_t)t * rows + r];
      out[t * out_room + r] = bias ? v + bias[r] : v;
    }

  g->products++;
  return 0;
}
