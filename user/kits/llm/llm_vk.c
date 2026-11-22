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
