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
  LLM_VK_MMV_NC,     /* mul_mat_vec_nc_f16_f32: attention's products */
  LLM_VK_OPS
};

struct llm_vk {
  struct kvk *v;
  struct kvk_compute op[LLM_VK_OPS];
  struct kvk_compute mmv[LLM_VK_MMV_KINDS];
  uint32_t mmv_rows[LLM_VK_MMV_KINDS];   /* rows a workgroup makes */
  uint32_t mmv_lanes[LLM_VK_MMV_KINDS];  /* and the lanes it has */
  /* The question's (V5), by the same kinds: medium tiles, and small. */
  struct kvk_compute mm[LLM_VK_MMV_KINDS], mm_s[LLM_VK_MMV_KINDS];
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

/* Recorded, for `n` inputs at once (V5, a question's tokens): `out[t][r] =
 * sum over c of W[r][c] * x[t][c]`, x `n` rows of `cols` floats and out
 * `n` rows of `rows` - llama.cpp's `mul_mm`, in tiles of 64 by 64. 1, or 0
 * for a type or a shape there is no product for. */
int llm_vk_mm(struct llm_vk *g, uint32_t type, struct kvk_span w, uint32_t cols,
              uint32_t rows, struct kvk_span x, uint32_t n, struct kvk_span out);

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

/* `n` floats into F16 a stride apart: out[i * stride + at] = x[i]. The
 * values' cache is kept transposed this way, a position a column, so that
 * attention's second product reads it a row at a time. `at` under 65536. */
int llm_vk_cpy_f16_strided(struct llm_vk *g, struct kvk_span x, struct kvk_span out, uint32_t n,
                           uint32_t stride, uint32_t at);
/* The same for `tokens` rows of `n`, the next a column on:
 * out[i * stride + at + t] = x[t * n + i]. */
int llm_vk_cpy_f16_transposed(struct llm_vk *g, struct kvk_span x, struct kvk_span out, uint32_t n,
                              uint32_t tokens, uint32_t stride, uint32_t at);

/* Attention's products (`mul_mat_vec_nc`): for each of `channels` heads c,
 * out[c * rows + r] = sum over k < cols of A[(c / divisor) * a_channel +
 * r * a_row + k] * b[c * b_channel + k], A in F16. The keys' scores are
 * this with A the cache and `rows` the positions; the values' sum is this
 * with A the transposed cache and `cols` the positions. */
int llm_vk_mmv_nc(struct llm_vk *g, struct kvk_span a, struct kvk_span b, struct kvk_span out,
                  uint32_t cols, uint32_t rows, uint32_t a_row, uint32_t a_channel,
                  uint32_t b_channel, uint32_t divisor, uint32_t channels);

/* ------------------------------------------------------------------------ */
/* A model on the GPU (`docs/xavier.md` X12, V4)                              */
/* ------------------------------------------------------------------------ */

struct llm_model;

#define LLM_VK_PROFILE 48

/* A model's weights in storage buffers, its cache - keys a position a row,
 * values transposed - and the working vectors, all on the device. */
struct llm_vk_model {
  struct llm_vk *g;
  struct llm_model *m;
  struct kvk_buffer *w;                  /* by the tensor's index in `m->tensors` */
  struct kvk_buffer *kc, *vc;            /* a layer each */
  struct kvk_buffer x, xn, qkv, att, prob, aout, ffo, ff, up, mlp, logits, pos;
  /* The same for a question's `LLM_BATCH` tokens at once, a token a row. */
  struct kvk_buffer bx, bxn, bq, bk, bv, baout, bffo, bff, bup, bmlp, bpos;
  /* **Where a step's time goes** (V6), with the model's clock
   * (`llm_clock`): steps and batches taken, the clock's ticks recording
   * them and from submitting to done, and the device's own milliseconds
   * between the first command and the last. Summed until the caller
   * clears them. */
  uint64_t steps, record_ticks, submit_ticks;
  double gpu_ms;
  /* With `profile`, each step's GPU time by the line of `llm_vk.c` whose
   * barrier ended it: `LLM_VK_PROFILE` lines at most, summed over steps. */
  int profile;
  uint32_t prof_n, prof_line[LLM_VK_PROFILE];
  double prof_ms[LLM_VK_PROFILE];
  char why[160];
};

/* Everything copied up from an open model; 1, or 0 with `why`. */
int  llm_vk_model_open(struct llm_vk_model *vm, struct llm_vk *g, struct llm_model *m);
void llm_vk_model_close(struct llm_vk_model *vm);

/* One token through every layer, recorded as one command buffer and
 * submitted once: the logits into `m->logits`, the cache a position on, as
 * `llm_step` leaves them. 0, or -1. */
int llm_vk_step(struct llm_vk_model *vm, int32_t token);

/* **The vector product's shapes tried on this device** (V6): every lanes
 * and rows of a small grid over the feed-forward's weights and the
 * logits', each timed by the device and held to the answer of the shape
 * that is built, a line each to `said`. For a profile, not for use: it
 * makes some sixty pipelines and takes seconds. Changes nothing. */
typedef void llm_vk_said_fn(void *ctx, const char *line);
void llm_vk_tune(struct llm_vk_model *vm, llm_vk_said_fn *said, void *ctx);

/* Past this many tokens a batch's products are matrix products, and up to
 * it vector products a token at a time, as ggml's `mul_mat_vec_max_cols`. */
#define LLM_VK_MM_MIN 8

/* `n` tokens of a question through every layer at once (V5), up to
 * `LLM_BATCH`: every product a matrix product, `llm_vk_mm`, so a weight is
 * read once for all of them, and each token's attention over the
 * positions up to its own. The last token's logits into `m->logits` and
 * the cache `n` positions on, as `llm_step_batch` leaves them. 0, or -1. */
int llm_vk_batch(struct llm_vk_model *vm, const int32_t *tokens, uint32_t n);

#endif
