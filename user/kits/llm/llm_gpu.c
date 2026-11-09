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
  g->share[0] = g->share[1] = 512;

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
  g->tiled_bytes = 0;
}

/* A buffer of the device's of at least `bytes`, grown when it is not. */
static const char *room_for(struct llm_gpu *g, size_t bytes, int read_back, uint32_t *id, void **cpu,
                            size_t *have)
{
  const char *no;
  size_t want = *have ? *have : 64u * 1024u;

  if (bytes <= *have) return NULL;

  while (want < bytes) want *= 2;

  if (*have) g->dev.forget(g->dev.ctx, *id);

  *have = 0;

  if ((no = g->dev.buffer(g->dev.ctx, want, read_back, id, cpu)) != NULL) return no;

  *have = want;
  return NULL;
}

/*
 * The weight's buffer: made the first time it is used, and kept. **Laid out
 * for the GPU** (`llmgpu.lua`, *Why the tiles*): the rows in tiles of 16,
 * and for each pair of blocks its nine dwords, each the tile's sixteen rows'
 * side by side - so a read of the kernel's is one cache line rather than
 * sixteen. Written in the order it lies, which is what write-combining
 * memory wants; the last tile's missing rows are nought.
 */
static const char *weight(struct llm_gpu *g, const struct llm_tensor *w, uint32_t *id)
{
  uint64_t rows = w->ne[1], blocks = w->ne[0] / LLM_QK, pairs = blocks / 2;
  uint64_t tiles = (rows + LLMGPU_TILE - 1) / LLMGPU_TILE;
  size_t bytes = (size_t)(tiles * pairs * 9u * LLMGPU_TILE * 4u);
  const uint8_t *from = w->data;
  uint32_t *to;
  void *cpu;
  const char *no;

  for (uint32_t i = 0; i < g->ntensors; i++) {
    if (g->tensor[i].at == w->data) {
      *id = g->tensor[i].id;
      return NULL;
    }
  }

  if (g->ntensors == LLM_GPU_TENSORS) return "more weights than the GPU path keeps";

  if ((no = g->dev.buffer(g->dev.ctx, bytes, 0, id, &cpu)) != NULL) return no;

  to = cpu;

  for (uint64_t t = 0; t < tiles; t++)
    for (uint64_t p = 0; p < pairs; p++)
      for (uint32_t j = 0; j < 9; j++)
        for (uint32_t l = 0; l < LLMGPU_TILE; l++) {
          uint64_t r = t * LLMGPU_TILE + l;
          uint32_t d = 0;

          if (r < rows) memcpy(&d, from + r * blocks * 18u + p * 36u + j * 4u, 4);
          *to++ = d;
        }

  g->tensor[g->ntensors].at = w->data;
  g->tensor[g->ntensors].id = *id;
  g->tensor[g->ntensors].bytes = bytes;
  g->ntensors++;
  g->tiled_bytes += bytes;
  return NULL;
}

int64_t llm_gpu_product(void *ctx, const struct llm_tensor *w, const uint8_t *xq, size_t xq_room,
                        const float *xs, size_t xs_room, uint32_t n, const float *bias,
                        float *out, size_t out_room)
{
  struct llm_gpu *g = ctx;
  uint64_t cols = w->ne[0], all = w->ne[1], blocks = cols / LLM_QK, rows;
  uint32_t wid, ids[4];
  const char *no;

  if (w->type != LLM_Q4_0) return declined(g, "not a Q4_0 weight");
  if (blocks == 0 || blocks % 2 != 0) return declined(g, "an odd number of blocks to a row");
  if (all == 0 || all > 0x7fffffffu / 4u || n == 0) return declined(g, "no rows");

  /* The GPU's rows: its share, in whole tiles, the first of them. */
  rows = all * g->share[n > 1] / 1024u / LLMGPU_TILE * LLMGPU_TILE;
  if (rows == 0) rows = LLMGPU_TILE;
  if (rows > all) rows = all;

  if ((no = weight(g, w, &wid)) != NULL
      || (no = room_for(g, (size_t)n * blocks * 32u, 0, &g->xq_id, &g->xq, &g->xq_bytes)) != NULL
      || (no = room_for(g, (size_t)n * blocks * 8u, 0, &g->xm_id, &g->xm, &g->xm_bytes)) != NULL
      || (no = room_for(g, (size_t)n * rows * 4u, 1, &g->out_id, &g->out, &g->out_bytes)) != NULL)
    return declined(g, no);

  /* The inputs as the kernel reads them: Q8_0's blocks less their scales,
   * so every value is in whole dwords; and for each pair of blocks a
   * record of 16 bytes, the two scales as floats and each block's values
   * summed, for the nibbles' "less eight" (`llmgpu.lua`, `xm`). */
  for (uint32_t t = 0; t < n; t++) {
    uint8_t *to = (uint8_t *)g->xq + (size_t)t * blocks * 32u;
    uint32_t *rec = (uint32_t *)g->xm + (size_t)t * blocks * 2u;
    const uint8_t *from = xq + t * xq_room;
    const float *scale = xs + t * xs_room;

    for (uint64_t b = 0; b < blocks; b++) {
      const int8_t *v = (const int8_t *)(from + b * 34u + 2u);
      int32_t total = 0;

      memcpy(to + b * 32u, v, 32u);
      for (uint32_t i = 0; i < 32; i++) total += v[i];

      memcpy(&rec[(b / 2) * 4 + (b & 1)], &scale[b], 4);
      memcpy(&rec[(b / 2) * 4 + 2 + (b & 1)], &total, 4);
    }
  }

  int32_t constants[4];
  constants[LLMGPU_Q4_0_ROWS] = (int32_t)rows;
  constants[LLMGPU_Q4_0_BLOCKS] = (int32_t)blocks;
  constants[LLMGPU_Q4_0_W_AT] = 0;
  constants[LLMGPU_Q4_0_INPUTS] = (int32_t)n;

  ids[LLMGPU_Q4_0_W] = wid;
  ids[LLMGPU_Q4_0_XQ] = g->xq_id;
  ids[LLMGPU_Q4_0_XM] = g->xm_id;
  ids[LLMGPU_Q4_0_OUT] = g->out_id;

  if ((no = g->dev.run(g->dev.ctx, g->kernel, g->simd, (uint32_t)((rows + g->simd - 1) / g->simd), ids, 4,
                       constants, sizeof constants)) != NULL)
    return declined(g, no);

  g->pending.on = 1;
  g->pending.out = out;
  g->pending.out_room = out_room;
  g->pending.bias = bias;
  g->pending.n = n;
  g->pending.rows = (uint32_t)rows;
  g->products++;
  g->rows_gpu += rows * n;
  g->rows_all += all * n;
  return (int64_t)rows;
}

int llm_gpu_done(void *ctx)
{
  struct llm_gpu *g = ctx;
  uint32_t n = g->pending.n, rows = g->pending.rows, *share = &g->share[n > 1];
  int already = 0;
  const char *no;

  if (!g->pending.on) return -1;

  g->pending.on = 0;

  if ((no = g->dev.wait(g->dev.ctx, &already)) != NULL) {
    (void)declined(g, no);
    return -1;
  }

  /* The share moved for the next one: more when the GPU had finished
   * first, less when the processor had. */
  if (already) *share = *share + LLM_GPU_SHARE_STEP > 1024u ? 1024u : *share + LLM_GPU_SHARE_STEP;
  else *share = *share > 2u * LLM_GPU_SHARE_STEP ? *share - LLM_GPU_SHARE_STEP : LLM_GPU_SHARE_STEP;

  /* The sums, and the bias added as the processor adds it. */
  const float *sums = g->out;
  float *out = g->pending.out;
  const float *bias = g->pending.bias;

  for (uint32_t t = 0; t < n; t++)
    for (uint32_t r = 0; r < rows; r++) {
      float v = sums[(size_t)t * rows + r];
      out[t * g->pending.out_room + r] = bias ? v + bias[r] : v;
    }

  return 0;
}
