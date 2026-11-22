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

/* `vk_mat_mat_push_constants`: `mul_mm`'s, 68 bytes. The shader reads the
 * first sixteen; `padded_N` is what the release sends after them. */
struct mm_push {
  uint32_t M, N, K, stride_a, stride_b, stride_d;
  uint32_t batch_stride_a, batch_stride_b, batch_stride_d;
  uint32_t base_work_group_z, num_batches, k_split;
  uint32_t ne02, ne12, broadcast2, broadcast3;
  uint32_t padded_N;
};
_Static_assert(sizeof(struct mm_push) == 68, "vk_mat_mat_push_constants is 68 bytes");

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

int llm_vk_cpy_f16_transposed(struct llm_vk *g, struct kvk_span x, struct kvk_span out, uint32_t n,
                              uint32_t tokens, uint32_t stride, uint32_t at)
{
  if (n == 0 || tokens == 0 || stride == 0 || at + tokens > 0x10000) {
    return 0;
  }
  /* The source `tokens` rows of `n`; the destination's first index a
   * stride apart and its second one along, and `at` its misalignment in
   * elements (`get_doffset`), which is how ggml says where a view starts
   * inside a binding. */
  struct unary_push p = unary_flat(n, tokens);
  p.nb10 = stride;
  p.nb11 = 1;
  p.nb12 = p.nb13 = stride * n;
  p.misalign_offsets = at;
  struct kvk_span spans[2] = { x, out };
  elementwise(g, LLM_VK_CPY_F16, spans, &p, n * tokens);
  return 1;
}

int llm_vk_cpy_f16_strided(struct llm_vk *g, struct kvk_span x, struct kvk_span out, uint32_t n,
                           uint32_t stride, uint32_t at)
{
  return llm_vk_cpy_f16_transposed(g, x, out, n, 1, stride, at);
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
    kvk_compute_free(g->v, &g->mm[k]);
    kvk_compute_free(g->v, &g->mm_s[k]);
  }
  for (int k = 0; k < LLM_VK_OPS; k++) {
    kvk_compute_free(g->v, &g->op[k]);
  }
  g->v = NULL;
}

/* The vector products' shaders, by kind. */
static const struct { const uint32_t *code; size_t bytes; } SPV[LLM_VK_MMV_KINDS] = {
  { llmvk_mul_mat_vec_f32_f32_f32, sizeof(llmvk_mul_mat_vec_f32_f32_f32) },
  { llmvk_mul_mat_vec_f16_f32_f32, sizeof(llmvk_mul_mat_vec_f16_f32_f32) },
  { llmvk_mul_mat_vec_q4_0_f32_f32, sizeof(llmvk_mul_mat_vec_q4_0_f32_f32) },
  { llmvk_mul_mat_vec_q8_0_f32_f32, sizeof(llmvk_mul_mat_vec_q8_0_f32_f32) },
  { llmvk_mul_mat_vec_q6_k_f32_f32, sizeof(llmvk_mul_mat_vec_q6_k_f32_f32) },
};

/* How a workgroup's lanes are summed (V6): through shared memory or, for
 * Q4_0, by `subgroupAdd` and then shared memory across subgroups. ggml's
 * third, by `subgroupAdd` alone, hung an HD 530 (`testing.md` 18.992). */
enum { SUM_SHARED, SUM_ADD, SUMS };
static const char *const SUM_NAME[SUMS] = { "shared", "add" };

/* Kind `k`'s vector product, `lanes` a workgroup making `rows` rows. */
static int mmv_make(struct kvk *v, struct kvk_compute *c, int k, int sum, uint32_t lanes, uint32_t rows)
{
  const uint32_t spec[3] = { lanes, rows, 1 };
  const int add = sum == SUM_ADD && k == LLM_VK_MMV_Q4_0;

  return kvk_compute_make(v, c, add ? llmvk_mul_mat_vec_q4_0_add : SPV[k].code,
                          add ? sizeof(llmvk_mul_mat_vec_q4_0_add) : SPV[k].bytes,
                          5, sizeof(struct mmv_push), spec, 3, "mmv");
}

static int is_intel(const char *device)
{
  return strncmp(device, "Intel", 5) == 0;
}

int llm_vk_by_default(const char *device)
{
  return !is_intel(device);
}

int llm_vk_open(struct llm_vk *g, struct kvk *v)
{
  /* The products' pipelines, with the rows a workgroup makes that
   * `ggml-vulkan.cpp` chooses (`rm_stdq`, `rm_kq`): two of Q4_0 and one
   * of Q8_0 a workgroup, twice that on an Intel GPU; one of F32, two of
   * F16, two of Q6_K. A workgroup is four subgroups (`DMMV_WG_SIZE_LARGE`,
   * summed through shared memory), Q6_K's subgroups at least sixteen.
   *
   * **Intel's are measured, not ggml's** (V6, `llm_vk_tune`, `testing.md`
   * 18.992). On an HD 530: Q4_0 by subgroups at 64 lanes by 8 rows, 10.5 ms
   * where 128 by 4 through shared memory took 17.0; Q8_0 at 16 by 8, 45.3
   * where 128 by 2 took 65.3; Q6_K at 16 by 8, 120 where 128 by 2 took 224. */
  const uint32_t intel = (uint32_t)is_intel(v->device_name);
  const uint32_t rm_stdq = intel ? 2 : 1;
  const uint32_t sub = v->subgroup_size;
  const uint32_t sub16 = sub < 16 ? 16 : sub;
  const uint32_t rows[LLM_VK_MMV_KINDS] = { 1, 2, 2 * rm_stdq, 1 * rm_stdq, 2 };

  memset(g, 0, sizeof(*g));
  g->v = v;
  g->lanes = sub * 4;
  for (int k = 0; k < LLM_VK_MMV_KINDS; k++) {
    uint32_t lanes = (k == LLM_VK_MMV_Q6_K ? sub16 : sub) * 4, each = rows[k];
    int sum = SUM_SHARED;
    if (intel && k >= LLM_VK_MMV_Q4_0) {
      lanes = k == LLM_VK_MMV_Q4_0 ? 64 : 16;
      sum = k == LLM_VK_MMV_Q4_0 ? SUM_ADD : SUM_SHARED;
      each = 8;
    }
    g->mmv_lanes[k] = lanes;
    g->mmv_rows[k] = each;
    if (!mmv_make(v, &g->mmv[k], k, sum, lanes, each)) {
      snprintf(g->why, sizeof(g->why), "the product's pipeline %d: %s", k, v->why);
      llm_vk_close(g);
      return 0;
    }
  }

  /* The question's products (V5): `mul_mm` with `ggml_vk_load_shaders`'s
   * medium tiles for a device without matrix cores - 64 by 64, 128 lanes,
   * warps of `mm_warp_8` (the subgroup, at least 8 and at most 64), each
   * making 32 by 32 in two passes of 4 by 2 - floats' K steps of 16 and the
   * quantised types' of 32; then ALIGNED, always 0 here, so B is read a
   * value at a time; and the quantised type, `MmTypeA`, for the one shader
   * they share. */
  const uint32_t warp = sub < 8 ? 8 : sub > 64 ? 64 : sub;
  static const uint32_t MM_TYPE[LLM_VK_MMV_KINDS] = { 0, 1, 2, 8, 14 };   /* ggml's type ids */
  for (int k = 0; k < LLM_VK_MMV_KINDS; k++) {
    const int quant = k >= LLM_VK_MMV_Q4_0;
    uint32_t spec[13] = { 128, 64, 64, quant ? 32 : 16, warp, 32, 2, 4, 2, 1, warp, 0,
                          MM_TYPE[k] };
    /* And a small tile, 32 by 32, for n up to 32 - every batch of a
     * question - where a medium tile's 64 tokens would be mostly padding:
     * warps of WM = min(warp, 32) rows by 32, as many as cover 32 rows, each
     * in two passes of 2 by 2, which makes a warp's lanes exactly
     * (WSUBM / TM) by (WSUBN / TN) at every width from 8 to 64. */
    const uint32_t wm = warp < 32 ? warp : 32;
    uint32_t small[13] = { warp * (32 / wm), 32, 32, quant ? 32 : 16, wm, 32, 2, 2, 2, 1, warp, 0,
                           MM_TYPE[k] };
    const uint32_t *code = quant ? llmvk_matmul_quant_f32_fp32
                         : k == LLM_VK_MMV_F16 ? llmvk_matmul_f16_f32_fp32 : llmvk_matmul_f32_f32_fp32;
    size_t bytes = quant ? sizeof(llmvk_matmul_quant_f32_fp32)
                 : k == LLM_VK_MMV_F16 ? sizeof(llmvk_matmul_f16_f32_fp32)
                                       : sizeof(llmvk_matmul_f32_f32_fp32);

    if (!kvk_compute_make(v, &g->mm[k], code, bytes, 3, sizeof(struct mm_push), spec,
                          quant ? 13 : 12, "mm") ||
        !kvk_compute_make(v, &g->mm_s[k], code, bytes, 3, sizeof(struct mm_push), small,
                          quant ? 13 : 12, "mm small")) {
      snprintf(g->why, sizeof(g->why), "the question's product %d: %s", k, v->why);
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

int llm_vk_mm(struct llm_vk *g, uint32_t type, struct kvk_span w, uint32_t cols,
              uint32_t rows, struct kvk_span x, uint32_t n, struct kvk_span out)
{
  int k = llm_vk_mmv_kind(type);

  if (k < 0 || cols == 0 || rows == 0 || n == 0 || cols % BLOCK[k] != 0) {
    return 0;
  }

  /* One batch, no split of K: the strides are a row's values for W and x
   * and the output's rows for out, as `ggml_vk_matmul` gives them. */
  struct mm_push p = {
    .M = rows, .N = n, .K = cols, .stride_a = cols, .stride_b = cols, .stride_d = rows,
    .batch_stride_a = cols * rows, .batch_stride_b = cols * n, .batch_stride_d = rows * n,
    .num_batches = 1, .k_split = cols, .ne02 = 1, .ne12 = 1, .broadcast2 = 1, .broadcast3 = 1,
    .padded_N = n,
  };
  struct kvk_span spans[3] = { w, x, out };

  const uint32_t tile = n <= 32 ? 32 : 64;   /* `ggml_vk_guess_matmul_pipeline`'s small or medium */
  if ((rows + tile - 1) / tile > g->v->max_groups[0] || (n + tile - 1) / tile > g->v->max_groups[1]) {
    return 0;
  }
  kvk_compute_dispatch(g->v, tile == 32 ? &g->mm_s[k] : &g->mm[k], spans, &p, (rows + tile - 1) / tile,
                       (n + tile - 1) / tile, 1);
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
                                &vm->ffo, &vm->ff, &vm->up, &vm->mlp, &vm->logits, &vm->pos,
                                &vm->bx, &vm->bxn, &vm->bq, &vm->bk, &vm->bv, &vm->baout,
                                &vm->bffo, &vm->bff, &vm->bup, &vm->bmlp, &vm->bpos };
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
    { &vm->bx, (uint64_t)LLM_BATCH * E }, { &vm->bxn, (uint64_t)LLM_BATCH * E },
    { &vm->bq, (uint64_t)LLM_BATCH * E }, { &vm->bk, (uint64_t)LLM_BATCH * KV },
    { &vm->bv, (uint64_t)LLM_BATCH * KV }, { &vm->baout, (uint64_t)LLM_BATCH * E },
    { &vm->bffo, (uint64_t)LLM_BATCH * E }, { &vm->bff, (uint64_t)LLM_BATCH * F },
    { &vm->bup, (uint64_t)LLM_BATCH * F }, { &vm->bmlp, (uint64_t)LLM_BATCH * E },
    { &vm->bpos, LLM_BATCH },
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
                      struct kvk_span x, struct kvk_span out, uint32_t rows)
{
  struct llm_vk *g = vm->g;
  const struct llm_model *m = vm->m;
  if (w->type != LLM_F32 || (b && b->type != LLM_F32)) {
    return 0;
  }
  if (m->kind == LLM_ARCH_QWEN2) {
    return llm_vk_rms_norm(g, x, whole(tensor_buffer(vm, w)), out, m->n_embd, rows, m->norm_eps);
  }
  int ok = llm_vk_norm(g, x, out, m->n_embd, rows, m->norm_eps);
  kvk_compute_barrier(g->v);
  ok = ok && llm_vk_mul(g, out, whole(tensor_buffer(vm, w)), out, m->n_embd, rows, 1);
  if (b) {
    kvk_compute_barrier(g->v);
    ok = ok && llm_vk_add(g, out, whole(tensor_buffer(vm, b)), out, m->n_embd, rows, 1);
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
  const uint64_t t0 = m->clock ? m->clock() : 0;
  if (!ok || !kvk_begin(v)) {
    snprintf(vm->why, sizeof(vm->why), "%s", ok ? v->why : "a vector off the device's alignment");
    return -1;
  }
  /* With `profile`, a mark after each barrier, and the line it is on. */
  static uint32_t tag[KVK_MARKS];
  uint32_t tags = 0;
#define B() do { kvk_compute_barrier(v); \
                 if (vm->profile && tags < KVK_MARKS) { kvk_mark(v); tag[tags++] = __LINE__; } } while (0)
  kvk_time_start(v);
  if (vm->profile) {
    kvk_marks_start(v);
  }

  for (uint32_t l = 0; l < m->n_layer && ok; l++) {
    const struct llm_layer *L = &m->layers[l];

    ok = ok && layer_norm(vm, L->norm_w, L->norm_b, x, xn, 1);
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
      ok = ok && layer_norm(vm, L->ffn_norm_w, NULL, x, xn, 1);
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

  ok = ok && layer_norm(vm, m->out_norm_w, m->out_norm_b, x, xn, 1);
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
  kvk_time_end(v);
  const uint64_t t1 = m->clock ? m->clock() : 0;
  if (!kvk_submit(v)) {
    snprintf(vm->why, sizeof(vm->why), "submitting: %s", v->why);
    return -1;
  }
  const uint64_t t2 = m->clock ? m->clock() : 0;
  const double gpu = kvk_time_ms(v);
  vm->steps++;
  vm->record_ticks += t1 - t0;
  vm->submit_ticks += t2 - t1;
  vm->gpu_ms += gpu > 0 ? gpu : 0;
  if (vm->profile) {
    static double ms[KVK_MARKS];
    uint32_t n = kvk_marks_read(v, ms, tags);
    for (uint32_t i = 0; i < n; i++) {
      uint32_t at = 0;
      while (at < vm->prof_n && vm->prof_line[at] != tag[i]) {
        at++;
      }
      if (at == vm->prof_n && vm->prof_n < LLM_VK_PROFILE) {
        vm->prof_line[vm->prof_n] = tag[i];
        vm->prof_ms[vm->prof_n++] = 0;
      }
      if (at < vm->prof_n) {
        vm->prof_ms[at] += ms[i];
      }
    }
  }
  memcpy(m->logits, vm->logits.bytes, (size_t)m->n_vocab * 4);
  m->n_past++;
  return 0;
}

/* Rows `first` to `first + rows` of a weight, by `n` tokens of `x`, into
 * `out` a token a row of `rows`, and the bias's same rows added: a fused
 * weight - phi-2's Q, K and V in one - read as the three it holds. */
static int product_rows(struct llm_vk_model *vm, const struct llm_tensor *w, const struct llm_tensor *bias,
                        uint32_t first, uint32_t rows, struct kvk_span x, uint32_t n, struct kvk_span out)
{
  struct kvk *v = vm->g->v;
  const uint32_t cols = (uint32_t)w->ne[0];
  const uint64_t rb = w->bytes / (uint64_t)w->ne[1];
  const VkDeviceSize at = rb * first, bat = (VkDeviceSize)first * 4;

  if ((v->storage_align && (at % v->storage_align != 0 || (bias && bat % v->storage_align != 0))) ||
      first + rows > (uint32_t)w->ne[1]) {
    snprintf(vm->why, sizeof(vm->why), "a weight's rows off the device's alignment");
    return 0;
  }
  struct kvk_span ws = { tensor_buffer(vm, w)->buffer, at, rb * rows };
  if (n > LLM_VK_MM_MIN) {
    if (!llm_vk_mm(vm->g, w->type, ws, cols, rows, x, n, out)) {
      return 0;
    }
  } else {
    /* Up to `mul_mat_vec_max_cols` tokens, ggml's vector product: a tile
     * of 32 tokens would be mostly padding, and cost as much as 32. */
    int ok = 1;
    for (uint32_t t = 0; t < n && ok; t++) {
      struct kvk_span xt = { x.buffer, x.offset + (VkDeviceSize)t * cols * 4, (VkDeviceSize)cols * 4 };
      struct kvk_span ot = { out.buffer, out.offset + (VkDeviceSize)t * rows * 4, (VkDeviceSize)rows * 4 };
      if (v->storage_align && (xt.offset % v->storage_align != 0 || ot.offset % v->storage_align != 0)) {
        snprintf(vm->why, sizeof(vm->why), "a token's row off the device's alignment");
        return 0;
      }
      ok = llm_vk_mmv(vm->g, w->type, ws, cols, rows, xt, ot);
    }
    if (!ok) {
      return 0;
    }
  }
  if (bias) {
    if (bias->type != LLM_F32) {
      return 0;
    }
    struct kvk_span bs = { tensor_buffer(vm, bias)->buffer, bat, (VkDeviceSize)rows * 4 };
    kvk_compute_barrier(v);
    return llm_vk_add(vm->g, out, bs, out, rows, n, 1);
  }
  return 1;
}

static int product_all(struct llm_vk_model *vm, const struct llm_tensor *w, const struct llm_tensor *bias,
                       struct kvk_span x, uint32_t n, struct kvk_span out)
{
  return product_rows(vm, w, bias, 0, (uint32_t)w->ne[1], x, n, out);
}

int llm_vk_batch(struct llm_vk_model *vm, const int32_t *tokens, uint32_t T)
{
  struct llm_model *m = vm->m;
  struct llm_vk *g = vm->g;
  struct kvk *v = g->v;
  const uint32_t E = m->n_embd, D = m->head_dim, KV = m->kv_dim, H = m->n_head, F = m->n_ff;
  const uint32_t pos = m->n_past;
  const int qwen = m->kind == LLM_ARCH_QWEN2;
  int ok = 1;

  if (T == 0 || T > LLM_BATCH || pos + T > m->n_ctx) {
    return -1;
  }
  for (uint32_t t = 0; t < T; t++) {
    if (tokens[t] < 0 || (uint32_t)tokens[t] >= m->n_vocab) {
      return -1;
    }
    llm_embedding(m, tokens[t], (float *)vm->bx.bytes + (size_t)t * E);
    ((int32_t *)vm->bpos.bytes)[t] = (int32_t)(pos + t);
  }

  struct kvk_span x = whole(&vm->bx), xn = whole(&vm->bxn), q = whole(&vm->bq);
  struct kvk_span k = whole(&vm->bk), vv = whole(&vm->bv), ffo = whole(&vm->bffo);
  struct kvk_span ff = whole(&vm->bff), up = whole(&vm->bup), mlp = whole(&vm->bmlp);
  struct kvk_span aout = whole(&vm->baout), bpos = whole(&vm->bpos);
  struct kvk_span att = whole(&vm->att), prob = whole(&vm->prob);
  struct kvk_span last = part(vm, &vm->bx, (uint64_t)(T - 1) * E, E, &ok);
  VkDeviceSize krow = (VkDeviceSize)pos * KV * 2;
  if (!ok || (v->storage_align && krow % v->storage_align != 0)) {
    snprintf(vm->why, sizeof(vm->why), "a row off the device's alignment");
    return -1;
  }
  const uint64_t t0 = m->clock ? m->clock() : 0;
  if (!kvk_begin(v)) {
    snprintf(vm->why, sizeof(vm->why), "%s", v->why);
    return -1;
  }
#define B() kvk_compute_barrier(v)
  kvk_time_start(v);

  for (uint32_t l = 0; l < m->n_layer && ok; l++) {
    const struct llm_layer *L = &m->layers[l];

    ok = ok && layer_norm(vm, L->norm_w, L->norm_b, x, xn, T);
    B();
    if (L->qkv_w) {
      ok = ok && product_rows(vm, L->qkv_w, L->qkv_b, 0, E, xn, T, q) &&
           product_rows(vm, L->qkv_w, L->qkv_b, E, KV, xn, T, k) &&
           product_rows(vm, L->qkv_w, L->qkv_b, E + KV, KV, xn, T, vv);
    } else {
      ok = ok && product_all(vm, L->q_w, L->q_b, xn, T, q) && product_all(vm, L->k_w, L->k_b, xn, T, k) &&
           product_all(vm, L->v_w, L->v_b, xn, T, vv);
    }
    B();
    ok = ok && llm_vk_rope_neox(g, q, bpos, q, D, H, T, m->n_rot, m->rope_base) &&
         llm_vk_rope_neox(g, k, bpos, k, D, m->n_head_kv, T, m->n_rot, m->rope_base);
    B();
    if (!qwen) {
      ok = ok && llm_vk_scale(g, q, q, E * T, 1.0f / sqrtf((float)D), 0);
    }

    /* The batch's keys into their rows, its values down their columns. */
    struct kvk_span kc = { vm->kc[l].buffer, krow, (VkDeviceSize)T * KV * 2 };
    ok = ok && llm_vk_cpy_f16(g, k, kc, T * KV) &&
         llm_vk_cpy_f16_transposed(g, vv, whole(&vm->vc[l]), KV, T, m->n_ctx, pos);
    B();

    /* Each token's attention over the positions up to its own: the
     * processor's, a token at a time, with no mask to build. */
    for (uint32_t t = 0; t < T && ok; t++) {
      const uint32_t n = pos + t + 1;
      struct kvk_span qt = part(vm, &vm->bq, (uint64_t)t * E, E, &ok);
      struct kvk_span at = part(vm, &vm->baout, (uint64_t)t * E, E, &ok);
      ok = ok && llm_vk_mmv_nc(g, whole(&vm->kc[l]), qt, att, D, n, KV, D, D, H / m->n_head_kv, H);
      B();
      ok = ok && llm_vk_soft_max(g, att, prob, n, H, qwen ? 1.0f / sqrtf((float)D) : 1.0f);
      B();
      ok = ok && llm_vk_mmv_nc(g, whole(&vm->vc[l]), prob, at, n, D, m->n_ctx, D * m->n_ctx, n,
                               H / m->n_head_kv, H);
      B();
    }

    ok = ok && product_all(vm, L->o_w, L->o_b, aout, T, ffo);
    if (qwen) {
      B();
      ok = ok && llm_vk_add(g, ffo, x, x, E, T, T);
      B();
      ok = ok && layer_norm(vm, L->ffn_norm_w, NULL, x, xn, T);
      B();
      ok = ok && product_all(vm, L->gate_w, NULL, xn, T, ff) && product_all(vm, L->up_w, NULL, xn, T, up);
      B();
      ok = ok && llm_vk_swiglu(g, ff, up, ff, F * T);
      B();
      ok = ok && product_all(vm, L->down_w, NULL, ff, T, mlp);
      B();
      ok = ok && llm_vk_add(g, mlp, x, x, E, T, T);
    } else {
      ok = ok && product_all(vm, L->up_w, L->up_b, xn, T, ff);
      B();
      ok = ok && llm_vk_gelu(g, ff, ff, F * T);
      B();
      ok = ok && product_all(vm, L->down_w, L->down_b, ff, T, mlp);
      B();
      ok = ok && llm_vk_add(g, ffo, mlp, ffo, E, T, T);
      B();
      ok = ok && llm_vk_add(g, ffo, x, x, E, T, T);
    }
    B();
  }

  /* Only the last token's logits are wanted. */
  ok = ok && layer_norm(vm, m->out_norm_w, m->out_norm_b, last, whole(&vm->xn), 1);
  B();
  ok = ok && product(vm, m->out_w, m->out_b, whole(&vm->xn), whole(&vm->logits));
  B();
#undef B

  if (!ok) {
    if (vm->why[0] == 0) {
      snprintf(vm->why, sizeof(vm->why), "a batch step there is no pipeline or shape for");
    }
    kvk_submit(v);
    return -1;
  }
  kvk_time_end(v);
  const uint64_t t1 = m->clock ? m->clock() : 0;
  if (!kvk_submit(v)) {
    snprintf(vm->why, sizeof(vm->why), "submitting: %s", v->why);
    return -1;
  }
  const uint64_t t2 = m->clock ? m->clock() : 0;
  const double gpu = kvk_time_ms(v);
  vm->steps++;
  vm->record_ticks += t1 - t0;
  vm->submit_ticks += t2 - t1;
  vm->gpu_ms += gpu > 0 ? gpu : 0;
  memcpy(m->logits, vm->logits.bytes, (size_t)m->n_vocab * 4);
  m->n_past += T;
  return 0;
}

/* One weight's sweep: `count` layers' worth of it one after another, timed
 * by the device, as it is built and then at every shape. */
static void tune_weight(struct llm_vk_model *vm, const struct llm_tensor **w, uint32_t count,
                        const char *name, struct kvk_span out, const float *got, float *want,
                        llm_vk_said_fn *said, void *ctx)
{
  /* Each sum's lanes, and the rows each is tried at. */
  static const uint32_t LANES[SUMS][3] = { { 16, 32, 64 }, { 16, 64, 128 } };
  static const uint32_t ROWS[4] = { 2, 4, 8, 16 };
  struct llm_vk *g = vm->g;
  struct kvk *v = g->v;
  const int k = llm_vk_mmv_kind(w[0]->type);
  const uint32_t cols = (uint32_t)w[0]->ne[0], n = (uint32_t)w[0]->ne[1];
  const int shapes = (k == LLM_VK_MMV_Q4_0 ? SUMS : 1) * 3 * 4;
  char line[192];

  if (k < 0) {
    return;
  }
  const struct kvk_compute built = g->mmv[k];
  const uint32_t built_rows = g->mmv_rows[k];
  double built_ms = 0;

  for (int i = -1; i < shapes; i++) {         /* -1: as it is built */
    struct kvk_compute c;
    uint32_t lanes = 0, rows = built_rows;
    memset(&c, 0, sizeof(c));
    const char *sum = "as built";
    if (i >= 0) {
      lanes = LANES[i / 12][i / 4 % 3];
      rows = ROWS[i % 4];
      sum = SUM_NAME[i / 12];
      if (!mmv_make(v, &c, k, i / 12, lanes, rows)) {
        snprintf(line, sizeof(line), "%s, %s, %u lanes %u rows: no pipeline: %s", name, sum, lanes, rows, v->why);
        said(ctx, line);
        continue;
      }
      g->mmv[k] = c;
      g->mmv_rows[k] = rows;
    }

    /* A layer a submission: the driver resets the GPU when a program's
     * fence has not moved for three seconds (GPU_HANG_MS), and a poor
     * shape over every layer at once is longer than that. */
    int ok = 1;
    double ms = 0;
    /* The first pass is thrown away: a driver may compile a pipeline as it
     * first runs it, as lavapipe does, and that is not the shape's time. */
    ok = kvk_begin(v);
    if (ok) {
      ok = llm_vk_mmv(g, w[0]->type, whole(tensor_buffer(vm, w[0])), cols, n, whole(&vm->x), out);
      ok = kvk_submit(v) && ok;
    }
    for (uint32_t j = 0; j < count && ok; j++) {
      ok = kvk_begin(v);
      if (ok) {
        kvk_time_start(v);
        ok = llm_vk_mmv(g, w[j]->type, whole(tensor_buffer(vm, w[j])), cols, n, whole(&vm->x), out);
        kvk_compute_barrier(v);
        kvk_time_end(v);
        ok = kvk_submit(v) && ok;
        ms += kvk_time_ms(v);
      }
    }

    if (!ok) {
      snprintf(line, sizeof(line), "%s, %s, %u lanes %u rows: it would not run", name, sum, lanes, rows);
    } else {
      double err = 0, all = 0;
      for (uint32_t r = 0; r < n; r++) {
        if (i < 0) {
          want[r] = got[r];
        }
        const double d = (double)got[r] - want[r];
        err += d * d;
        all += (double)want[r] * want[r];
      }
      /* Judged against what is built, which the host's tests hold to double
       * precision: a faster shape with another answer is no shape at all. */
      const int same = err <= 1e-7 * all;
      if (i < 0) {
        built_ms = ms;
        snprintf(line, sizeof(line), "%s %ux%u, as built (%u lanes %u rows): %llu us", name, n, cols,
                 g->mmv_lanes[k], rows,
                 (unsigned long long)(ms * 1000));
      } else {
        snprintf(line, sizeof(line), "%s, %s, %u lanes %u rows: %llu us%s", name, sum, lanes, rows,
                 (unsigned long long)(ms * 1000), same ? "" : ", ANOTHER ANSWER");
      }
    }
    said(ctx, line);

    if (i >= 0) {
      g->mmv[k] = built;
      g->mmv_rows[k] = built_rows;
      kvk_compute_free(v, &c);
      if (!ok) {
        break;                                /* a GPU that refused one is asked no more */
      }
      if (ms > 8 * built_ms) {
        i = (i / 4) * 4 + 3;                  /* more rows only slower: the next lanes */
      }
    }
  }
}

/* The width alone (V7): each layer's up, the same bytes of it read as a
 * narrower matrix of more rows and as a wider one of fewer - the same
 * arithmetic, and a longer input to keep in the GPU's caches. */
static void tune_width(struct llm_vk_model *vm, const struct llm_tensor **w, uint32_t count,
                       llm_vk_said_fn *said, void *ctx)
{
  static const uint32_t WIDTH[] = { 256, 512, 768, 896, 1024, 1152, 1280, 1536, 2048, 3072 };
  struct kvk *v = vm->g->v;
  const uint64_t values = w[0]->ne[0] * w[0]->ne[1];
  char line[192];

  if (llm_vk_mmv_kind(w[0]->type) != LLM_VK_MMV_Q4_0) {
    return;
  }
  for (uint32_t i = 0; i < sizeof(WIDTH) / sizeof(WIDTH[0]); i++) {
    const uint32_t rows = (uint32_t)(values / WIDTH[i]);
    double ms = 0;
    int ok = 1;

    if (rows > vm->m->n_vocab || WIDTH[i] > LLM_BATCH * vm->m->n_embd) {
      continue;                               /* no room for its input or its sums */
    }
    for (uint32_t c = 0; c < WIDTH[i]; c++) {
      ((float *)vm->bx.bytes)[c] = (float)((c * 7919u) % 1000u) / 500.0f - 1.0f;
    }
    for (uint32_t pass = 0; pass < 2 && ok; pass++) {
      ms = 0;                                 /* the first warms, as below */
      for (uint32_t j = 0; j < count && ok; j++) {
        ok = kvk_begin(v);
        if (ok) {
          kvk_time_start(v);
          ok = llm_vk_mmv(vm->g, w[j]->type, whole(tensor_buffer(vm, w[j])), WIDTH[i], rows, whole(&vm->bx),
                          whole(&vm->logits));
          kvk_compute_barrier(v);
          kvk_time_end(v);
          ok = kvk_submit(v) && ok;
          ms += kvk_time_ms(v);
        }
      }
    }
    if (ok) {
      snprintf(line, sizeof(line), "width %u, %u rows of every layer's up: %llu us, %llu MB/s of weights", WIDTH[i],
               rows, (unsigned long long)(ms * 1000),
               (unsigned long long)(ms > 0 ? values * count * 18 / 32 / (ms * 1000) : 0));
    } else {
      snprintf(line, sizeof(line), "width %u: it would not run", WIDTH[i]);
    }
    said(ctx, line);
  }
}

void llm_vk_tune(struct llm_vk_model *vm, llm_vk_said_fn *said, void *ctx)
{
  const struct llm_model *m = vm->m;
  /* The logits three times over: one dispatch alone is short to time. */
  const struct llm_tensor *ff[128], *out[3] = { m->out_w, m->out_w, m->out_w };
  const uint32_t layers = m->n_layer < 128 ? m->n_layer : 128;
  const uint32_t most = m->n_vocab > m->n_ff ? m->n_vocab : m->n_ff;
  float *want = malloc((size_t)most * 4);

  if (!want) {
    said(ctx, "the sweep: no memory");
    return;
  }
  for (uint32_t c = 0; c < m->n_embd; c++) {
    ((float *)vm->x.bytes)[c] = (float)((c * 7919u) % 1000u) / 500.0f - 1.0f;
  }
  for (uint32_t l = 0; l < layers; l++) {
    ff[l] = m->layers[l].up_w;
  }
  tune_width(vm, ff, layers, said, ctx);
  tune_weight(vm, ff, layers, "every layer's up", whole(&vm->ff), vm->ff.bytes, want, said, ctx);
  tune_weight(vm, out, 3, "the logits thrice", whole(&vm->logits), vm->logits.bytes, want, said, ctx);
  free(want);
}
