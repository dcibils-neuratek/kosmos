/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The LLM Kit's tensors on Vulkan** (`docs/xavier.md` X12): the
 * operations a layer is made of, each recorded into the Vulkan Kit's
 * command buffer as one dispatch of one of llama.cpp's own shaders
 * (`tools/vkshaders.py` compiles them; `llmvk_shaders.h`). A token is a
 * recording of many of these with a barrier between each step, submitted
 * once.
 *
 * Every operation reads and writes spans of storage buffers the caller
 * made (`kvk_buffer_make_storage`); nothing here allocates while
 * recording. Each one's push constants and dispatch are the ones
 * `ggml-vulkan.cpp` gives the same shader for the same shapes, since the
 * shaders were written against exactly those.
 *
 *   struct llm_vk g;
 *   if (llm_vk_open(&g, &v)) {
 *       kvk_begin(&v);
 *       llm_vk_mmv(&g, LLM_Q4_0, w, cols, rows, x, out);
 *       kvk_compute_barrier(&v);
 *       kvk_submit(&v);
 *   }
 */
#ifndef KOSMOS_LLM_VK_H
#define KOSMOS_LLM_VK_H

#include <stdint.h>

#include "kits/vulkan/kvk.h"

/* The products' weight types, in the order the pipelines are kept. */
enum {
  LLM_VK_MMV_F32,
  LLM_VK_MMV_F16,
  LLM_VK_MMV_Q4_0,
  LLM_VK_MMV_Q8_0,
  LLM_VK_MMV_Q6_K,
  LLM_VK_MMV_KINDS
};

/* A layer's other steps, one pipeline each. */
enum {
  LLM_VK_NORM,       /* norm_f32: (x - mean) / sqrt(var + eps), a row at a time */
  LLM_VK_RMS_NORM,   /* rms_norm_f32, the weight multiplied in (`rms_norm_mul`) */
  LLM_VK_ADD,        /* add_f32_f32_f32, b repeated over a's rows */
  LLM_VK_MUL,        /* mul_f32_f32_f32, the same */
  LLM_VK_SCALE,      /* scale_f32: x * s + b */
  LLM_VK_GELU,       /* gelu_f32, GELU's tanh form */
  LLM_VK_SILU,       /* silu_f32 */
  LLM_VK_SWIGLU,     /* swiglu_f32, split: silu(gate) * up */
  LLM_VK_SOFT_MAX,   /* soft_max_f32, no mask, a row at a time */
  LLM_VK_ROPE_NEOX,  /* rope_neox_f32 */
  LLM_VK_CPY_F16,    /* cpy_f32_f16 */
  LLM_VK_CPY_F32,    /* cpy_f32_f32 */
  LLM_VK_OPS
};

struct llm_vk {
  struct kvk *v;
  struct kvk_compute op[LLM_VK_OPS];
  struct kvk_compute mmv[LLM_VK_MMV_KINDS];
  uint32_t mmv_rows[LLM_VK_MMV_KINDS];   /* rows a workgroup makes */
  uint32_t lanes;                        /* lanes a workgroup, for the products */
  char why[128];
};

/* The pipelines made on a device opened with KVK_COMPUTE: 1, or 0 with
 * `why`. */
int  llm_vk_open(struct llm_vk *g, struct kvk *v);
void llm_vk_close(struct llm_vk *g);

/* The pipeline for a ggml type (`LLM_F32`, ...): its LLM_VK_MMV_ index, or
 * -1 for a type there is no product for. */
int llm_vk_mmv_kind(uint32_t type);

/* Recorded: `out[r] = sum over c of W[r][c] * x[c]` for `rows` rows of
 * `cols` values, W in `type` (a row is `cols` values, whole blocks of its
 * type), x and out floats. 1, or 0 for a type or a shape there is no
 * product for. */
int llm_vk_mmv(struct llm_vk *g, uint32_t type, struct kvk_span w, uint32_t cols,
               uint32_t rows, struct kvk_span x, struct kvk_span out);

/* Every step below is recorded, as the product is, over floats laid out
 * row after row, `cols` a row; 1, or 0 for a shape there is none for. */

/* `out` = each row of `x` less its mean, over its deviation (`eps` in it).
 * Phi-2's layer norm is this, then MUL by the weight and ADD the bias. */
int llm_vk_norm(struct llm_vk *g, struct kvk_span x, struct kvk_span out, uint32_t cols,
                uint32_t rows, float eps);

/* `out` = each row of `x` over its root mean square, times `w` (one row):
 * Qwen2's norm. */
int llm_vk_rms_norm(struct llm_vk *g, struct kvk_span x, struct kvk_span w, struct kvk_span out,
                    uint32_t cols, uint32_t rows, float eps);

/* `out` = a + b, or a * b, where `a` and `out` are `rows` rows and `b` is
 * `b_rows` of them - `rows`, or 1 to be repeated down every row. */
int llm_vk_add(struct llm_vk *g, struct kvk_span a, struct kvk_span b, struct kvk_span out,
               uint32_t cols, uint32_t rows, uint32_t b_rows);
int llm_vk_mul(struct llm_vk *g, struct kvk_span a, struct kvk_span b, struct kvk_span out,
               uint32_t cols, uint32_t rows, uint32_t b_rows);

/* `out` = x * s + b, for `n` floats. */
int llm_vk_scale(struct llm_vk *g, struct kvk_span x, struct kvk_span out, uint32_t n, float s,
                 float b);

/* `out` = gelu(x) or silu(x), for `n` floats. */
int llm_vk_gelu(struct llm_vk *g, struct kvk_span x, struct kvk_span out, uint32_t n);
int llm_vk_silu(struct llm_vk *g, struct kvk_span x, struct kvk_span out, uint32_t n);

/* `out` = silu(gate) * up, for `n` floats: SwiGLU's middle. */
int llm_vk_swiglu(struct llm_vk *g, struct kvk_span gate, struct kvk_span up, struct kvk_span out,
                  uint32_t n);

/* `out` = softmax of each row of `x * scale`. */
int llm_vk_soft_max(struct llm_vk *g, struct kvk_span x, struct kvk_span out, uint32_t cols,
                    uint32_t rows, float scale);

/* RoPE, NeoX's pairing, on `x`: `tokens` of `heads` heads of `head` floats,
 * the first `n_rot` of each head turned by the angle of its token's position
 * - `pos`, one int32 a token - and the rest copied. */
int llm_vk_rope_neox(struct llm_vk *g, struct kvk_span x, struct kvk_span pos, struct kvk_span out,
                     uint32_t head, uint32_t heads, uint32_t tokens, uint32_t n_rot, float base);

/* `n` floats copied to F16 (rounded to the nearest), or to floats. */
int llm_vk_cpy_f16(struct llm_vk *g, struct kvk_span x, struct kvk_span out, uint32_t n);
int llm_vk_cpy_f32(struct llm_vk *g, struct kvk_span x, struct kvk_span out, uint32_t n);

#endif
