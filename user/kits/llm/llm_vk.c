/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The LLM Kit's tensors on Vulkan** (`llm_vk.h`, `docs/xavier.md` X12).
 * Each operation is one of llama.cpp's shaders, given what
 * `ggml-vulkan.cpp` gives it - its specialisation constants when the
 * pipeline is made, its push constants and workgroups when it is recorded -
 * for the shapes a model of ours has: one token, or a batch, and tensors
 * laid out row after row.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm_core.h"
#include "llm_vk.h"
#include "llmvk_shaders.h"

/* `vk_mat_vec_push_constants`, as `ggml-vulkan-push-constants.h` has it. */
struct mmv_push {
  uint32_t ncols, stride_a, stride_b, stride_d;
  uint32_t batch_stride_a, batch_stride_b, batch_stride_d;
  uint32_t fusion_flags, base_work_group_y;
  uint32_t ne02, ne12, broadcast2, broadcast3;
};

/* `vk_op_unary_push_constants`: a source and a destination, each four
 * extents and four strides in elements, and the magic numbers the shader
 * divides an index by (`init_pushconst_fastdiv`). */
struct unary_push {
  uint32_t ne;
  uint32_t ne00, ne01, ne02, ne03, nb00, nb01, nb02, nb03;
  uint32_t ne10, ne11, ne12, ne13, nb10, nb11, nb12, nb13;
  uint32_t misalign_offsets;
  float param1, param2, param3, param4;
  uint32_t ne0_012mp, ne0_01mp, ne0_0mp, ne0_Ls;
  uint32_t ne1_012mp, ne1_01mp, ne1_0mp, ne1_Ls;
};

/* `vk_op_binary_push_constants`: two sources and a destination. */
struct binary_push {
  uint32_t ne;
  uint32_t ne00, ne01, ne02, ne03, nb00, nb01, nb02, nb03;
  uint32_t ne10, ne11, ne12, ne13, nb10, nb11, nb12, nb13;
  uint32_t ne20, ne21, ne22, ne23, nb20, nb21, nb22, nb23;
  uint32_t misalign_offsets;
  float param1, param2;
  int32_t param3;
};

/* `vk_op_glu_push_constants`. */
struct glu_push {
  uint32_t N, ne00, ne20, mode;
  float alpha, limit;
  uint32_t nb00, nb01, nb02, nb03, nb10, nb11, nb12, nb13, nb20, nb21, nb22, nb23;
  uint32_t ne21, ne22, misalign_offsets;
  uint32_t ne2_012mp, ne2_012L, ne2_01mp, ne2_01L, ne2_0mp, ne2_0L;
};

/* `vk_op_soft_max_push_constants`. */
struct soft_max_push {
  uint32_t KX, KY, ne00, ne01, ne02, ne12, ne13, nb11, nb12, nb13;
  float scale, max_bias, m0, m1;
  uint32_t n_head_log2, nrows_x, has_sinks;
};

/* `vk_op_rope_push_constants`, which is `rope_params.glsl`'s struct. */
struct rope_push {
  uint32_t rope_mode, nrows, n_dims, n_offs;
  float freq_scale, freq_base, ext_factor, attn_factor;
  float corr_dims[2];
  float theta_scale;
  uint32_t has_ff;
  int32_t sections[4];
  uint32_t is_imrope, is_back, set_rows_stride;
  uint32_t ne00, ne01, ne02, nb01, nb02, nb03, nb11, nb12, nb13;
  uint32_t a_offset, d_offset;
};

/* `vk_mat_vec_nc_push_constants`. */
struct nc_push {
  uint32_t ncols_x, nrows_x, row_stride_x, channel_stride_x, channel_stride_y, channel_x_divisor;
  uint32_t ne12, b_offset, d_offset, nb03, nb13, nb23, fusion_flags;
};

_Static_assert(sizeof(struct nc_push) == 52, "the nc product's push constants are 52 bytes");
_Static_assert(sizeof(struct unary_push) == 120, "the unary push constants are 120 bytes");
_Static_assert(sizeof(struct binary_push) == 116, "the binary push constants are 116 bytes");
_Static_assert(sizeof(struct glu_push) == 108, "the GLU push constants are 108 bytes");
_Static_assert(sizeof(struct soft_max_push) == 68, "the softmax push constants are 68 bytes");
_Static_assert(sizeof(struct rope_push) == 120, "the RoPE push constants are 120 bytes");

/* `init_fastdiv_values`: L = ceil(log2(d)), and the multiplier that makes
 * a division by `d` a multiply-high and a shift. */
static void fastdiv(uint32_t d, uint32_t *mp, uint32_t *l)
{
  uint32_t L = 0;
  while (L < 32 && ((uint32_t)1 << L) < d) {
    L++;
  }
  *l = L;
  *mp = (uint32_t)(((uint64_t)1 << 32) * (((uint64_t)1 << L) - d) / d + 1);
}

/* A unary step over `n` floats in one contiguous row, source and
 * destination alike, as `vk_op_unary_push_constants_init` and
 * `init_pushconst_fastdiv` make them for a 1-D tensor. */
static struct unary_push unary_flat(uint32_t ne00, uint32_t ne01)
{
  struct unary_push p = {
    .ne = ne00 * ne01,
    .ne00 = ne00, .ne01 = ne01, .ne02 = 1, .ne03 = 1,
    .nb00 = 1, .nb01 = ne00, .nb02 = ne00 * ne01, .nb03 = ne00 * ne01,
    .ne10 = ne00, .ne11 = ne01, .ne12 = 1, .ne13 = 1,
    .nb10 = 1, .nb11 = ne00, .nb12 = ne00 * ne01, .nb13 = ne00 * ne01,
  };
  uint32_t l012, l01, l0, m012, m01, m0;

  fastdiv(p.ne02 * p.ne01 * p.ne00, &p.ne0_012mp, &l012);
  fastdiv(p.ne01 * p.ne00, &p.ne0_01mp, &l01);
  fastdiv(p.ne00, &p.ne0_0mp, &l0);
  p.ne0_Ls = l012 | (l01 << 8) | (l0 << 16);
  fastdiv(p.ne12 * p.ne11 * p.ne10, &p.ne1_012mp, &m012);
  fastdiv(p.ne11 * p.ne10, &p.ne1_01mp, &m01);
  fastdiv(p.ne10, &p.ne1_0mp, &m0);
  p.ne1_Ls = m012 | (m01 << 8) | (m0 << 16);
  return p;
}

/* `a` and the destination `rows` of `cols`, `b` `b_rows` of them. */
static struct binary_push binary_rows(uint32_t cols, uint32_t rows, uint32_t b_rows)
{
  struct binary_push p = {
    .ne = cols * rows,
    .ne00 = cols, .ne01 = rows, .ne02 = 1, .ne03 = 1,
    .nb00 = 1, .nb01 = cols, .nb02 = cols * rows, .nb03 = cols * rows,
    .ne10 = cols, .ne11 = b_rows, .ne12 = 1, .ne13 = 1,
    .nb10 = 1, .nb11 = cols, .nb12 = cols * b_rows, .nb13 = cols * b_rows,
    .ne20 = cols, .ne21 = rows, .ne22 = 1, .ne23 = 1,
    .nb20 = 1, .nb21 = cols, .nb22 = cols * rows, .nb23 = cols * rows,
  };
  return p;
}

/* The workgroups `ggml_vk_op_f32` gives a step over `ne` elements, its
 * `wg_denoms` 512 across: one across, the rest in 512s down and in
 * 262144s deep, which is what `get_idx` reads back. */
static void elementwise(struct llm_vk *g, int op, const struct kvk_span *spans, const void *push,
                        uint32_t ne)
{
  uint32_t y = 1, z = 1;
  if (ne > 262144) {
    y = 512;
    z = (ne + 262143) / 262144;
  } else if (ne > 512) {
    y = (ne + 511) / 512;
  }
  kvk_compute_dispatch(g->v, &g->op[op], spans, push, 1, y, z);
}

/* And a step a row a workgroup: rows across, and past 512 of them, down. */
static void rowwise(struct llm_vk *g, int op, const struct kvk_span *spans, const void *push,
                    uint32_t nr)
{
  if (nr > 262144) {
    kvk_compute_dispatch(g->v, &g->op[op], spans, push, 512, 512, (nr + 262143) / 262144);
  } else if (nr > 512) {
    kvk_compute_dispatch(g->v, &g->op[op], spans, push, 512, (nr + 511) / 512, 1);
  } else {
    kvk_compute_dispatch(g->v, &g->op[op], spans, push, nr, 1, 1);
  }
}

int llm_vk_norm(struct llm_vk *g, struct kvk_span x, struct kvk_span out, uint32_t cols,
                uint32_t rows, float eps)
{
  if (cols == 0 || rows == 0) {
    return 0;
  }
  struct unary_push p = unary_flat(cols, rows);
  p.param1 = eps;
  struct kvk_span spans[2] = { x, out };
  rowwise(g, LLM_VK_NORM, spans, &p, rows);
  return 1;
}

int llm_vk_rms_norm(struct llm_vk *g, struct kvk_span x, struct kvk_span w, struct kvk_span out,
                    uint32_t cols, uint32_t rows, float eps)
{
  if (cols == 0 || rows == 0 || rows > g->v->max_groups[0]) {
    return 0;
  }
  /* `ggml_vk_rms_norm_push_constants`, with the weight as the second
   * source - one row, repeated - and no partial sums. */
  struct binary_push p = binary_rows(cols, rows, 1);
  p.param1 = eps;
  struct kvk_span spans[4] = { x, w, out, x };
  kvk_compute_dispatch(g->v, &g->op[LLM_VK_RMS_NORM], spans, &p, rows, 1, 1);
  return 1;
}

static int binary(struct llm_vk *g, int op, struct kvk_span a, struct kvk_span b, struct kvk_span out,
                  uint32_t cols, uint32_t rows, uint32_t b_rows)
{
  if (cols == 0 || rows == 0 || (b_rows != rows && b_rows != 1)) {
    return 0;
  }
  struct binary_push p = binary_rows(cols, rows, b_rows);
  /* ADD has a fourth binding, for the partial sums it can keep for an
   * RMS norm after it: given the source, as ggml gives it when there are
   * none. */
  struct kvk_span spans[4] = { a, b, out, a };
  elementwise(g, op, spans, &p, cols * rows);
  return 1;
}

int llm_vk_add(struct llm_vk *g, struct kvk_span a, struct kvk_span b, struct kvk_span out,
               uint32_t cols, uint32_t rows, uint32_t b_rows)
{
  return binary(g, LLM_VK_ADD, a, b, out, cols, rows, b_rows);
}

int llm_vk_mul(struct llm_vk *g, struct kvk_span a, struct kvk_span b, struct kvk_span out,
               uint32_t cols, uint32_t rows, uint32_t b_rows)
{
  return binary(g, LLM_VK_MUL, a, b, out, cols, rows, b_rows);
}

static int unary(struct llm_vk *g, int op, struct kvk_span x, struct kvk_span out, uint32_t n,
                 float param1, float param2)
{
  if (n == 0) {
    return 0;
  }
  struct unary_push p = unary_flat(n, 1);
  p.param1 = param1;
  p.param2 = param2;
  struct kvk_span spans[2] = { x, out };
  elementwise(g, op, spans, &p, n);
  return 1;
}

int llm_vk_scale(struct llm_vk *g, struct kvk_span x, struct kvk_span out, uint32_t n, float s,
                 float b)
{
  return unary(g, LLM_VK_SCALE, x, out, n, s, b);
}

int llm_vk_gelu(struct llm_vk *g, struct kvk_span x, struct kvk_span out, uint32_t n)
{
  return unary(g, LLM_VK_GELU, x, out, n, 0, 0);
}

int llm_vk_silu(struct llm_vk *g, struct kvk_span x, struct kvk_span out, uint32_t n)
{
  return unary(g, LLM_VK_SILU, x, out, n, 0, 0);
}

int llm_vk_cpy_f16(struct llm_vk *g, struct kvk_span x, struct kvk_span out, uint32_t n)
{
  return unary(g, LLM_VK_CPY_F16, x, out, n, 0, 0);
}

int llm_vk_cpy_f32(struct llm_vk *g, struct kvk_span x, struct kvk_span out, uint32_t n)
{
  return unary(g, LLM_VK_CPY_F32, x, out, n, 0, 0);
}

int llm_vk_swiglu(struct llm_vk *g, struct kvk_span gate, struct kvk_span up, struct kvk_span out,
                  uint32_t n)
{
  if (n == 0) {
    return 0;
  }
  /* `ggml_vk_glu` split (mode 2): the gate one tensor and up another, a
   * single row of `n` each. */
  struct glu_push p = {
    .N = n, .ne00 = n, .ne20 = n, .mode = 2,
    .nb00 = 1, .nb01 = n, .nb02 = n, .nb03 = n,
    .nb10 = 1, .nb11 = n, .nb12 = n, .nb13 = n,
    .nb20 = 1, .nb21 = n, .nb22 = n, .nb23 = n,
    .ne21 = 1, .ne22 = 1,
  };
  fastdiv(n, &p.ne2_012mp, &p.ne2_012L);
  fastdiv(n, &p.ne2_01mp, &p.ne2_01L);
  fastdiv(n, &p.ne2_0mp, &p.ne2_0L);
  struct kvk_span spans[3] = { gate, up, out };
  elementwise(g, LLM_VK_SWIGLU, spans, &p, n);
  return 1;
}

int llm_vk_soft_max(struct llm_vk *g, struct kvk_span x, struct kvk_span out, uint32_t cols,
                    uint32_t rows, float scale)
{
  if (cols == 0 || rows == 0 || cols > 16384) {
    return 0;
  }
  /* `ggml_vk_soft_max` with no mask and no sinks: a 2-D tensor, so one
   * head (`n_head_log2` 1), and no ALiBi (`max_bias` 0, `m0` and `m1` 1).
   * Past 16384 columns ggml takes three other shaders. */
  struct soft_max_push p = {
    .KX = cols, .KY = 0, .ne00 = cols, .ne01 = rows, .ne02 = 1,
    .scale = scale, .max_bias = 0, .m0 = 1, .m1 = 1,
    .n_head_log2 = 1, .nrows_x = rows, .has_sinks = 0,
  };
  struct kvk_span spans[4] = { x, x, x, out };
  rowwise(g, LLM_VK_SOFT_MAX, spans, &p, rows);
  return 1;
}

int llm_vk_rope_neox(struct llm_vk *g, struct kvk_span x, struct kvk_span pos, struct kvk_span out,
                     uint32_t head, uint32_t heads, uint32_t tokens, uint32_t n_rot, float base)
{
  if (head == 0 || heads == 0 || tokens == 0 || n_rot == 0 || n_rot > head || n_rot % 2 != 0 ||
      head % 2 != 0) {
    return 0;
  }
  /* `ggml_vk_make_rope_constants` for `GGML_ROPE_TYPE_NEOX` (2), with no
   * YaRN (`ext_factor` 0, so `corr_dims` are not read), no frequency
   * factors and nothing scaled: a head a row, a token's heads a plane, and
   * `rope_data_pos` read by the plane. */
  struct rope_push p = {
    .rope_mode = 2, .nrows = heads * tokens, .n_dims = n_rot, .n_offs = 0,
    .freq_scale = 1, .freq_base = base, .ext_factor = 0, .attn_factor = 1,
    .theta_scale = powf(base, -2.0f / (float)n_rot),
    .ne00 = head, .ne01 = heads, .ne02 = tokens,
    .nb01 = head, .nb02 = head * heads, .nb03 = head * heads * tokens,
    .nb11 = head, .nb12 = head * heads, .nb13 = head * heads * tokens,
  };
  struct kvk_span spans[5] = { x, pos, x, out, x };

  /* A row a workgroup across, pairs of a row 256 to a workgroup down
   * (`wg_denoms` 512 of them, two elements a thread); past the dispatch's
   * width, 32768 rows across and the rest deep. */
  uint32_t nrows = p.nrows, z = 1;
  if (nrows > g->v->max_groups[0]) {
    z = (nrows + 32767) / 32768;
    nrows = 32768;
  }
  kvk_compute_dispatch(g->v, &g->op[LLM_VK_ROPE_NEOX], spans, &p, nrows, (head + 511) / 512, z);
  return 1;
}

int llm_vk_cpy_f16_strided(struct llm_vk *g, struct kvk_span x, struct kvk_span out, uint32_t n,
                           uint32_t stride, uint32_t at)
{
  if (n == 0 || stride == 0 || at > 0xffff) {
    return 0;
  }
  /* The destination one row of `n` a stride apart, and `at` its
   * misalignment in elements (`get_doffset`), which is how ggml says
   * where a view starts inside a binding. */
  struct unary_push p = unary_flat(n, 1);
  p.nb10 = stride;
  p.nb11 = p.nb12 = p.nb13 = stride * n;
  p.misalign_offsets = at;
  struct kvk_span spans[2] = { x, out };
  elementwise(g, LLM_VK_CPY_F16, spans, &p, n);
  return 1;
}

int llm_vk_mmv_nc(struct llm_vk *g, struct kvk_span a, struct kvk_span b, struct kvk_span out,
                  uint32_t cols, uint32_t rows, uint32_t a_row, uint32_t a_channel,
                  uint32_t b_channel, uint32_t divisor, uint32_t channels)
{
  if (cols == 0 || rows == 0 || channels == 0 || divisor == 0 || rows > g->v->max_groups[1] ||
      channels > g->v->max_groups[2]) {
    return 0;
  }
  /* `ggml_vk_mul_mat_vec_nc_f16_f32`: one batch (`ne03`, `nb03` unused),
   * the positions or a head's values down, the heads deep. */
  struct nc_push p = {
    .ncols_x = cols, .nrows_x = rows, .row_stride_x = a_row, .channel_stride_x = a_channel,
    .channel_stride_y = b_channel, .channel_x_divisor = divisor, .ne12 = channels,
    .nb03 = a_channel * channels, .nb13 = b_channel * channels, .nb23 = rows * channels,
  };
  struct kvk_span spans[5] = { a, b, out, out, out };
  kvk_compute_dispatch(g->v, &g->op[LLM_VK_MMV_NC], spans, &p, 1, rows, channels);
  return 1;
}

int llm_vk_mmv_kind(uint32_t type)
{
  switch (type) {
  case LLM_F32:  return LLM_VK_MMV_F32;
  case LLM_F16:  return LLM_VK_MMV_F16;
  case LLM_Q4_0: return LLM_VK_MMV_Q4_0;
  case LLM_Q8_0: return LLM_VK_MMV_Q8_0;
  case LLM_Q6_K: return LLM_VK_MMV_Q6_K;
  default:       return -1;
  }
}

/* Values a block of each kind holds: a row is whole blocks. */
static const uint32_t BLOCK[LLM_VK_MMV_KINDS] = { 1, 1, LLM_QK, LLM_QK, LLM_QK_K };

void llm_vk_close(struct llm_vk *g)
{
  if (g->v == NULL) {
    return;
  }
  for (int k = 0; k < LLM_VK_MMV_KINDS; k++) {
    kvk_compute_free(g->v, &g->mmv[k]);
  }
  for (int k = 0; k < LLM_VK_OPS; k++) {
    kvk_compute_free(g->v, &g->op[k]);
  }
  g->v = NULL;
}

int llm_vk_open(struct llm_vk *g, struct kvk *v)
{
  /* The products' pipelines, with the rows a workgroup makes that
   * `ggml-vulkan.cpp` chooses (`rm_stdq`, `rm_kq`): two of Q4_0 and one
   * of Q8_0 a workgroup, twice that on an Intel GPU; one of F32, two of
   * F16, two of Q6_K. A workgroup is four subgroups (`DMMV_WG_SIZE_LARGE`,
   * summed through shared memory), Q6_K's subgroups at least sixteen. */
  const uint32_t intel = strncmp(v->device_name, "Intel", 5) == 0;
  const uint32_t rm_stdq = intel ? 2 : 1;
  const uint32_t sub = v->subgroup_size;
  const uint32_t sub16 = sub < 16 ? 16 : sub;
  static const struct { const uint32_t *code; size_t bytes; } SPV[LLM_VK_MMV_KINDS] = {
    { llmvk_mul_mat_vec_f32_f32_f32, sizeof(llmvk_mul_mat_vec_f32_f32_f32) },
    { llmvk_mul_mat_vec_f16_f32_f32, sizeof(llmvk_mul_mat_vec_f16_f32_f32) },
    { llmvk_mul_mat_vec_q4_0_f32_f32, sizeof(llmvk_mul_mat_vec_q4_0_f32_f32) },
    { llmvk_mul_mat_vec_q8_0_f32_f32, sizeof(llmvk_mul_mat_vec_q8_0_f32_f32) },
    { llmvk_mul_mat_vec_q6_k_f32_f32, sizeof(llmvk_mul_mat_vec_q6_k_f32_f32) },
  };
  const uint32_t rows[LLM_VK_MMV_KINDS] = { 1, 2, 2 * rm_stdq, 1 * rm_stdq, 2 };

  memset(g, 0, sizeof(*g));
  g->v = v;
  g->lanes = sub * 4;
  for (int k = 0; k < LLM_VK_MMV_KINDS; k++) {
    uint32_t spec[3] = { (k == LLM_VK_MMV_Q6_K ? sub16 : sub) * 4, rows[k], 1 };

    g->mmv_rows[k] = rows[k];
    if (!kvk_compute_make(v, &g->mmv[k], SPV[k].code, SPV[k].bytes, 5,
                          sizeof(struct mmv_push), spec, 3, "mmv")) {
      snprintf(g->why, sizeof(g->why), "the product's pipeline %d: %s", k, v->why);
      llm_vk_close(g);
      return 0;
    }
  }

  /* The other steps, with the bindings, push constants and specialisation
   * constants `ggml_vk_load_shaders` gives each. RMS norm's are `norepeat`
   * 0, `do_multiply` 1 and a workgroup of 512 - ggml keeps 128 and 256 as
   * well, for short rows, which are only quicker; softmax's workgroup is
   * a subgroup. */
  const uint32_t rms_spec[4] = { 0, 1, 0, 512 };
  const uint32_t repeat_spec[1] = { 0 };
  const uint32_t soft_max_spec[1] = { sub };
  static const struct { const uint32_t *code; size_t bytes; uint32_t buffers, push; } OPS[LLM_VK_OPS] = {
    [LLM_VK_NORM] = { llmvk_norm_f32, sizeof(llmvk_norm_f32), 2, sizeof(struct unary_push) },
    [LLM_VK_RMS_NORM] = { llmvk_rms_norm_f32, sizeof(llmvk_rms_norm_f32), 4, sizeof(struct binary_push) },
    [LLM_VK_ADD] = { llmvk_add_f32_f32_f32, sizeof(llmvk_add_f32_f32_f32), 4, sizeof(struct binary_push) },
    [LLM_VK_MUL] = { llmvk_mul_f32_f32_f32, sizeof(llmvk_mul_f32_f32_f32), 3, sizeof(struct binary_push) },
    [LLM_VK_SCALE] = { llmvk_scale_f32, sizeof(llmvk_scale_f32), 2, sizeof(struct unary_push) },
    [LLM_VK_GELU] = { llmvk_gelu_f32, sizeof(llmvk_gelu_f32), 2, sizeof(struct unary_push) },
    [LLM_VK_SILU] = { llmvk_silu_f32, sizeof(llmvk_silu_f32), 2, sizeof(struct unary_push) },
    [LLM_VK_SWIGLU] = { llmvk_swiglu_f32, sizeof(llmvk_swiglu_f32), 3, sizeof(struct glu_push) },
    [LLM_VK_SOFT_MAX] = { llmvk_soft_max_f32, sizeof(llmvk_soft_max_f32), 4, sizeof(struct soft_max_push) },
    [LLM_VK_ROPE_NEOX] = { llmvk_rope_neox_f32, sizeof(llmvk_rope_neox_f32), 5, sizeof(struct rope_push) },
    [LLM_VK_CPY_F16] = { llmvk_cpy_f32_f16, sizeof(llmvk_cpy_f32_f16), 2, sizeof(struct unary_push) },
    [LLM_VK_CPY_F32] = { llmvk_cpy_f32_f32, sizeof(llmvk_cpy_f32_f32), 2, sizeof(struct unary_push) },
    [LLM_VK_MMV_NC] = { llmvk_mul_mat_vec_nc_f16_f32, sizeof(llmvk_mul_mat_vec_nc_f16_f32), 5, sizeof(struct nc_push) },
  };
  for (int k = 0; k < LLM_VK_OPS; k++) {
    const uint32_t *spec = NULL;
    uint32_t nspec = 0;
    if (k == LLM_VK_RMS_NORM) {
      spec = rms_spec;
      nspec = 4;
    } else if (k == LLM_VK_ADD || k == LLM_VK_MUL) {
      spec = repeat_spec;
      nspec = 1;
    } else if (k == LLM_VK_SOFT_MAX) {
      spec = soft_max_spec;
      nspec = 1;
    }
    if (!kvk_compute_make(v, &g->op[k], OPS[k].code, OPS[k].bytes, OPS[k].buffers, OPS[k].push,
                          spec, nspec, "op")) {
      snprintf(g->why, sizeof(g->why), "the step's pipeline %d: %s", k, v->why);
      llm_vk_close(g);
      return 0;
    }
  }
  return 1;
}

int llm_vk_mmv(struct llm_vk *g, uint32_t type, struct kvk_span w, uint32_t cols,
               uint32_t rows, struct kvk_span x, struct kvk_span out)
{
  int k = llm_vk_mmv_kind(type);

  if (k < 0 || cols == 0 || rows == 0 || cols % BLOCK[k] != 0) {
    return 0;
  }

  /* One input (`ne11` 1) and no batch: the strides are a row's values and
   * the output's rows; the fused biases' slots given the output, as
   * `ggml-vulkan.cpp` gives them when nothing is fused. */
  struct mmv_push p = {
    .ncols = cols, .stride_a = cols, .stride_b = cols, .stride_d = rows,
    .batch_stride_a = cols * rows, .batch_stride_b = cols, .batch_stride_d = rows,
    .ne02 = 1, .ne12 = 1, .broadcast2 = 1, .broadcast3 = 1,
  };
  struct kvk_span spans[5] = { w, x, out, out, out };

  /* A row a workgroup's worth of elements, unless there are more rows than
   * a dispatch may have workgroups: then 64 ways in z, as ggml does. */
  uint32_t elements = rows, z = 1;
  if (elements > g->v->max_groups[0]) {
    z = 64;
    elements = (elements + z - 1) / z;
  }
  kvk_compute_dispatch(g->v, &g->mmv[k], spans, &p,
                       (elements + g->mmv_rows[k] - 1) / g->mmv_rows[k], 1, z);
  return 1;
}

/* ------------------------------------------------------------------------ */
/* A model on the GPU (V4)                                                    */
/* ------------------------------------------------------------------------ */

static struct kvk_span whole(const struct kvk_buffer *b)
{
  return (struct kvk_span){ b->buffer, 0, 0 };
}

/* `floats` floats into `b`, `at` floats in: an offset a binding may start
 * at only where it falls on the device's alignment, which every one taken
 * here does - a model's widths are multiples of 64. */
static struct kvk_span part(const struct llm_vk_model *vm, const struct kvk_buffer *b, uint64_t at,
                            uint64_t floats, int *ok)
{
  uint64_t bytes = at * 4;
  if (vm->g->v->storage_align && bytes % vm->g->v->storage_align != 0) {
    *ok = 0;
  }
  return (struct kvk_span){ b->buffer, bytes, floats * 4 };
}

static const struct kvk_buffer *tensor_buffer(const struct llm_vk_model *vm, const struct llm_tensor *t)
{
  return &vm->w[t - vm->m->tensors];
}

void llm_vk_model_close(struct llm_vk_model *vm)
{
  if (vm->g == NULL) {
    return;
  }
  struct kvk *v = vm->g->v;
  if (vm->w) {
    for (uint32_t i = 0; i < vm->m->n_tensors; i++) {
      kvk_buffer_free(v, &vm->w[i]);
    }
  }
  if (vm->kc) {
    for (uint32_t l = 0; l < vm->m->n_layer; l++) {
      kvk_buffer_free(v, &vm->kc[l]);
      kvk_buffer_free(v, &vm->vc[l]);
    }
  }
  struct kvk_buffer *vecs[] = { &vm->x, &vm->xn, &vm->qkv, &vm->att, &vm->prob, &vm->aout,
                                &vm->ffo, &vm->ff, &vm->up, &vm->mlp, &vm->logits, &vm->pos };
  for (size_t i = 0; i < sizeof(vecs) / sizeof(vecs[0]); i++) {
    kvk_buffer_free(v, vecs[i]);
  }
  free(vm->w);
  free(vm->kc);
  free(vm->vc);
  vm->w = vm->kc = vm->vc = NULL;
  vm->g = NULL;
}

int llm_vk_model_open(struct llm_vk_model *vm, struct llm_vk *g, struct llm_model *m)
{
  struct kvk *v = g->v;
  const uint32_t E = m->n_embd, KV = m->kv_dim, F = m->n_ff;

  memset(vm, 0, sizeof(*vm));
  vm->g = g;
  vm->m = m;
  if (m->n_ctx > 0xffff || m->head_dim % 2 != 0 || m->n_head % m->n_head_kv != 0) {
    snprintf(vm->why, sizeof(vm->why), "a shape the GPU path does not take: n_ctx %u, heads %u/%u",
             m->n_ctx, m->n_head, m->n_head_kv);
    return 0;
  }

  /* Every tensor its own buffer: one is bound whole to each step that
   * reads it, and no tensor of these models is past a binding's range. */
  vm->w = calloc(m->n_tensors, sizeof(*vm->w));
  vm->kc = calloc(m->n_layer, sizeof(*vm->kc));
  vm->vc = calloc(m->n_layer, sizeof(*vm->vc));
  if (!vm->w || !vm->kc || !vm->vc) {
    snprintf(vm->why, sizeof(vm->why), "no memory");
    llm_vk_model_close(vm);
    return 0;
  }
  for (uint32_t i = 0; i < m->n_tensors; i++) {
    const struct llm_tensor *t = &m->tensors[i];
    if (t == m->tok_embd && m->out_w != m->tok_embd) {
      continue;   /* read here, a row a token, by `llm_embedding` */
    }
    if (!kvk_buffer_make_storage(v, &vm->w[i], (t->bytes + 3) & ~(uint64_t)3, "weights")) {
      snprintf(vm->why, sizeof(vm->why), "a tensor's buffer, %llu bytes: %s",
               (unsigned long long)t->bytes, v->why);
      llm_vk_model_close(vm);
      return 0;
    }
    memcpy(vm->w[i].bytes, t->data, t->bytes);
  }

  /* The cache in F16: keys a position a row of KV, values transposed, KV
   * rows of a position each. */
  for (uint32_t l = 0; l < m->n_layer; l++) {
    VkDeviceSize bytes = (VkDeviceSize)m->n_ctx * KV * 2;
    if (!kvk_buffer_make_storage(v, &vm->kc[l], bytes, "keys") ||
        !kvk_buffer_make_storage(v, &vm->vc[l], bytes, "values")) {
      snprintf(vm->why, sizeof(vm->why), "the cache: %s", v->why);
      llm_vk_model_close(vm);
      return 0;
    }
    memset(vm->kc[l].bytes, 0, bytes);
    memset(vm->vc[l].bytes, 0, bytes);
  }

  const struct { struct kvk_buffer *b; uint64_t floats; } VECS[] = {
    { &vm->x, E }, { &vm->xn, E }, { &vm->qkv, 3 * (uint64_t)E },
    { &vm->att, (uint64_t)m->n_head * m->n_ctx }, { &vm->prob, (uint64_t)m->n_head * m->n_ctx },
    { &vm->aout, E }, { &vm->ffo, E }, { &vm->ff, F }, { &vm->up, F }, { &vm->mlp, E },
    { &vm->logits, m->n_vocab }, { &vm->pos, 1 },
  };
  for (size_t i = 0; i < sizeof(VECS) / sizeof(VECS[0]); i++) {
    if (!kvk_buffer_make_storage(v, VECS[i].b, VECS[i].floats * 4, "vector")) {
      snprintf(vm->why, sizeof(vm->why), "a working vector: %s", v->why);
      llm_vk_model_close(vm);
      return 0;
    }
  }
  return 1;
}

/* A product by a weight, and its bias where it has one: `out` rows of
 * the tensor's, `x` its columns. */
static int product(struct llm_vk_model *vm, const struct llm_tensor *w, const struct llm_tensor *bias,
                   struct kvk_span x, struct kvk_span out)
{
  const uint32_t cols = (uint32_t)w->ne[0], rows = (uint32_t)w->ne[1];
  if (!llm_vk_mmv(vm->g, w->type, whole(tensor_buffer(vm, w)), cols, rows, x, out)) {
    return 0;
  }
  if (bias) {
    if (bias->type != LLM_F32) {
      return 0;
    }
    kvk_compute_barrier(vm->g->v);
    return llm_vk_add(vm->g, out, whole(tensor_buffer(vm, bias)), out, rows, 1, 1);
  }
  return 1;
}

/* A model's norm, from `x` into `out`: RMS and the weight in Qwen2's,
 * LayerNorm, the weight and the bias in phi-2's. */
static int layer_norm(struct llm_vk_model *vm, const struct llm_tensor *w, const struct llm_tensor *b,
                      struct kvk_span x, struct kvk_span out)
{
  struct llm_vk *g = vm->g;
  const struct llm_model *m = vm->m;
  if (w->type != LLM_F32 || (b && b->type != LLM_F32)) {
    return 0;
  }
  if (m->kind == LLM_ARCH_QWEN2) {
    return llm_vk_rms_norm(g, x, whole(tensor_buffer(vm, w)), out, m->n_embd, 1, m->norm_eps);
  }
  int ok = llm_vk_norm(g, x, out, m->n_embd, 1, m->norm_eps);
  kvk_compute_barrier(g->v);
  ok = ok && llm_vk_mul(g, out, whole(tensor_buffer(vm, w)), out, m->n_embd, 1, 1);
  if (b) {
    kvk_compute_barrier(g->v);
    ok = ok && llm_vk_add(g, out, whole(tensor_buffer(vm, b)), out, m->n_embd, 1, 1);
  }
  return ok;
}

int llm_vk_step(struct llm_vk_model *vm, int32_t token)
{
  struct llm_model *m = vm->m;
  struct llm_vk *g = vm->g;
  struct kvk *v = g->v;
  const uint32_t E = m->n_embd, D = m->head_dim, KV = m->kv_dim, H = m->n_head;
  const uint32_t pos = m->n_past, n = pos + 1;
  const int qwen = m->kind == LLM_ARCH_QWEN2;
  int ok = 1;

  if (pos >= m->n_ctx || token < 0 || (uint32_t)token >= m->n_vocab) {
    return -1;
  }
  llm_embedding(m, token, (float *)vm->x.bytes);
  *(int32_t *)vm->pos.bytes = (int32_t)pos;

  struct kvk_span x = whole(&vm->x), xn = whole(&vm->xn), att = whole(&vm->att);
  struct kvk_span prob = whole(&vm->prob), aout = whole(&vm->aout), ffo = whole(&vm->ffo);
  struct kvk_span ff = whole(&vm->ff), up = whole(&vm->up), mlp = whole(&vm->mlp);
  struct kvk_span q = part(vm, &vm->qkv, 0, E, &ok);
  struct kvk_span k = part(vm, &vm->qkv, E, KV, &ok);
  struct kvk_span vv = part(vm, &vm->qkv, 2 * (uint64_t)E, KV, &ok);
  struct kvk_span qkv = part(vm, &vm->qkv, 0, 3 * (uint64_t)E, &ok);
  if (!ok || !kvk_begin(v)) {
    snprintf(vm->why, sizeof(vm->why), "%s", ok ? v->why : "a vector off the device's alignment");
    return -1;
  }
#define B() kvk_compute_barrier(v)

  for (uint32_t l = 0; l < m->n_layer && ok; l++) {
    const struct llm_layer *L = &m->layers[l];

    ok = ok && layer_norm(vm, L->norm_w, L->norm_b, x, xn);
    B();

    /* Q, K and V where the processor keeps them: K at E, V at 2E. */
    if (L->qkv_w) {
      ok = ok && product(vm, L->qkv_w, L->qkv_b, xn, qkv);
    } else {
      ok = ok && product(vm, L->q_w, L->q_b, xn, q) && product(vm, L->k_w, L->k_b, xn, k) &&
           product(vm, L->v_w, L->v_b, xn, vv);
    }
    B();

    ok = ok && llm_vk_rope_neox(g, q, whole(&vm->pos), q, D, H, 1, m->n_rot, m->rope_base) &&
         llm_vk_rope_neox(g, k, whole(&vm->pos), k, D, m->n_head_kv, 1, m->n_rot, m->rope_base);
    B();
    if (!qwen) {
      ok = ok && llm_vk_scale(g, q, q, E, 1.0f / sqrtf((float)D), 0);
    }

    /* This position's key into its row, its value down its column. */
    VkDeviceSize krow = (VkDeviceSize)pos * KV * 2;
    if (v->storage_align && krow % v->storage_align != 0) {
      snprintf(vm->why, sizeof(vm->why), "a key's row is off the device's alignment");
      ok = 0;
      break;
    }
    struct kvk_span kc = { vm->kc[l].buffer, krow, (VkDeviceSize)KV * 2 };
    ok = ok && llm_vk_cpy_f16(g, k, kc, KV) &&
         llm_vk_cpy_f16_strided(g, vv, whole(&vm->vc[l]), KV, m->n_ctx, pos);
    B();

    /* The scores, a head's row of `n` each, its softmax, and the values'
     * sum: a head a channel, a group's heads reading its key and value
     * head. */
    ok = ok && llm_vk_mmv_nc(g, whole(&vm->kc[l]), q, att, D, n, KV, D, D, H / m->n_head_kv, H);
    B();
    ok = ok && llm_vk_soft_max(g, att, prob, n, H, qwen ? 1.0f / sqrtf((float)D) : 1.0f);
    B();
    ok = ok && llm_vk_mmv_nc(g, whole(&vm->vc[l]), prob, aout, n, D, m->n_ctx, D * m->n_ctx, n,
                             H / m->n_head_kv, H);
    B();

    ok = ok && product(vm, L->o_w, L->o_b, aout, ffo);
    if (qwen) {
      B();
      ok = ok && llm_vk_add(g, ffo, x, x, E, 1, 1);
      B();
      ok = ok && layer_norm(vm, L->ffn_norm_w, NULL, x, xn);
      B();
      ok = ok && product(vm, L->gate_w, NULL, xn, ff) && product(vm, L->up_w, NULL, xn, up);
      B();
      ok = ok && llm_vk_swiglu(g, ff, up, ff, m->n_ff);
      B();
      ok = ok && product(vm, L->down_w, NULL, ff, mlp);
      B();
      ok = ok && llm_vk_add(g, mlp, x, x, E, 1, 1);
    } else {
      /* phi-2's MLP beside the attention, off the same norm; then the two
       * added, then the residual, ggml's order. */
      ok = ok && product(vm, L->up_w, L->up_b, xn, ff);
      B();
      ok = ok && llm_vk_gelu(g, ff, ff, m->n_ff);
      B();
      ok = ok && product(vm, L->down_w, L->down_b, ff, mlp);
      B();
      ok = ok && llm_vk_add(g, ffo, mlp, ffo, E, 1, 1);
      B();
      ok = ok && llm_vk_add(g, ffo, x, x, E, 1, 1);
    }
    B();
  }

  ok = ok && layer_norm(vm, m->out_norm_w, m->out_norm_b, x, xn);
  B();
  ok = ok && product(vm, m->out_w, m->out_b, xn, whole(&vm->logits));
  B();
#undef B

  if (!ok) {
    if (vm->why[0] == 0) {
      snprintf(vm->why, sizeof(vm->why), "a step there is no pipeline or shape for");
    }
    kvk_submit(v);   /* what was recorded is ended, and thrown away */
    return -1;
  }
  if (!kvk_submit(v)) {
    snprintf(vm->why, sizeof(vm->why), "submitting: %s", v->why);
    return -1;
  }
  memcpy(m->logits, vm->logits.bytes, (size_t)m->n_vocab * 4);
  m->n_past++;
  return 0;
}
