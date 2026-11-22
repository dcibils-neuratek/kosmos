/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The LLM Kit's operations on Vulkan, held** (`docs/xavier.md` X12, V3):
 * each of `llm_vk.c`'s operations run on this Mac's lavapipe - Homebrew's
 * Mesa, the same driver QEMU's Kosmos runs - over random inputs, and held
 * to the same arithmetic done here in double precision, by the normalised
 * mean squared error llama.cpp's `test-backend-ops` uses: the sum of the
 * squared differences over the sum of the squared answers.
 *
 * The products are held to 1e-7, far under `test-backend-ops`' 5e-4 for a
 * product, since nothing here quantises the input: what is left is the
 * order a float sum is taken in. Their weights are random blocks of each
 * type, scales and all, so a block read wrong - a nibble's half, a scale's
 * place, Q6_K's high bits - is a large error, not a small one.
 *
 * `--control` gives each product's pipeline the next type's blocks, read
 * as its own, which must break every one: it passes, and exits 0, only
 * when each product is outside its bound.
 *
 * Usage: test_llmvk [--control]
 */
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "llm_core.h"
#include "llm_vk.h"

static uint64_t rng = 0x9e3779b97f4a7c15ull;

static uint32_t next(void)
{
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;
  return (uint32_t)(rng >> 16);
}

static float uniform(void)
{
  return (float)next() / 4294967296.0f * 2.0f - 1.0f;
}

static uint16_t half(float f)
{
  _Float16 h = (_Float16)f;
  uint16_t bits;
  memcpy(&bits, &h, 2);
  return bits;
}

/* A row of `cols` values of `type`, random, into `out`; its values as
 * the shader must read them into `values`. */
static size_t row_bytes(uint32_t type, uint32_t cols)
{
  switch (type) {
  case LLM_F32:  return (size_t)cols * 4;
  case LLM_F16:  return (size_t)cols * 2;
  case LLM_Q4_0: return (size_t)cols / LLM_QK * 18;
  case LLM_Q8_0: return (size_t)cols / LLM_QK * 34;
  case LLM_Q6_K: return (size_t)cols / LLM_QK_K * LLM_Q6K_BYTES;
  }
  return 0;
}

static void make_row(uint32_t type, uint32_t cols, uint8_t *out, double *values)
{
  if (type == LLM_F32) {
    for (uint32_t c = 0; c < cols; c++) {
      float f = uniform();
      memcpy(out + 4 * c, &f, 4);
      values[c] = f;
    }
  } else if (type == LLM_F16) {
    for (uint32_t c = 0; c < cols; c++) {
      uint16_t h = half(uniform());
      memcpy(out + 2 * c, &h, 2);
      values[c] = llm_f16_to_f32(h);
    }
  } else if (type == LLM_Q4_0) {
    for (uint32_t b = 0; b < cols / LLM_QK; b++) {
      uint8_t *blk = out + 18 * b;
      uint16_t d = half(uniform() * 0.05f);
      memcpy(blk, &d, 2);
      for (int j = 0; j < 16; j++) {
        blk[2 + j] = (uint8_t)next();
        values[32 * b + j] = ((blk[2 + j] & 15) - 8) * (double)llm_f16_to_f32(d);
        values[32 * b + j + 16] = ((blk[2 + j] >> 4) - 8) * (double)llm_f16_to_f32(d);
      }
    }
  } else if (type == LLM_Q8_0) {
    for (uint32_t b = 0; b < cols / LLM_QK; b++) {
      uint8_t *blk = out + 34 * b;
      uint16_t d = half(uniform() * 0.01f);
      memcpy(blk, &d, 2);
      for (int j = 0; j < 32; j++) {
        blk[2 + j] = (uint8_t)next();
        values[32 * b + j] = (int8_t)blk[2 + j] * (double)llm_f16_to_f32(d);
      }
    }
  } else {
    /* Q6_K, as ggml's `dequantize_row_q6_K` reads it: `ql` 128, `qh` 64,
     * sixteen signed scales, the F16 scale last. */
    for (uint32_t b = 0; b < cols / LLM_QK_K; b++) {
      uint8_t *blk = out + LLM_Q6K_BYTES * b;
      for (int j = 0; j < 208; j++) {
        blk[j] = (uint8_t)next();
      }
      uint16_t d = half(uniform() * 0.002f);
      memcpy(blk + 208, &d, 2);
      const uint8_t *ql = blk, *qh = blk + 128;
      const int8_t *sc = (const int8_t *)(blk + 192);
      double *y = values + 256 * b;
      double dd = llm_f16_to_f32(d);
      for (int n = 0; n < 256; n += 128) {
        for (int l = 0; l < 32; l++) {
          int is = l / 16;
          int q1 = ((ql[l] & 15) | (((qh[l] >> 0) & 3) << 4)) - 32;
          int q2 = ((ql[l + 32] & 15) | (((qh[l] >> 2) & 3) << 4)) - 32;
          int q3 = ((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32;
          int q4 = ((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32;
          y[l] = dd * sc[is] * q1;
          y[l + 32] = dd * sc[is + 2] * q2;
          y[l + 64] = dd * sc[is + 4] * q3;
          y[l + 96] = dd * sc[is + 6] * q4;
        }
        y += 128;
        ql += 64;
        qh += 32;
        sc += 8;
      }
    }
  }
}

/* The normalised mean squared error of `got` against `want`. */
static double nmse(const float *got, const double *want, size_t n)
{
  double err = 0, norm = 0;
  for (size_t i = 0; i < n; i++) {
    double d = got[i] - want[i];
    err += d * d;
    norm += want[i] * want[i];
  }
  return norm > 0 ? err / norm : err;
}

static int failures;

static void verdict(const char *what, double e, double bound, int control)
{
  /* NaN is not within any bound: in the control it is a product broken,
   * as it must be, and anywhere else a failure. */
  int ok = control ? !(e <= bound) : e <= bound;
  printf("%s: %s, nmse %.3g (%s %.0e)\n", ok ? "PASS" : "FAIL", what, e,
         control ? "must be over" : "at most", bound);
  failures += !ok;
}

/* One product: `rows` rows of `cols` of `type`, against the reference.
 * With `as` not `type`, the blocks of `type` read by `as`'s pipeline. */
static void product(struct llm_vk *g, uint32_t type, uint32_t as, uint32_t cols, uint32_t rows)
{
  struct kvk *v = g->v;
  struct kvk_buffer w = { 0 }, x = { 0 }, out = { 0 };
  size_t rb = row_bytes(type, cols);
  double *values = malloc(sizeof(double) * cols);
  double *want = malloc(sizeof(double) * rows);
  double *xv = malloc(sizeof(double) * cols);
  char what[96];

  /* Twice the room: read as another type, a row can be up to twice as
   * long, and must still be inside the buffer. */
  if (!kvk_buffer_make_storage(v, &w, rb * rows * 2, "w") ||
      !kvk_buffer_make_storage(v, &x, (VkDeviceSize)cols * 4, "x") ||
      !kvk_buffer_make_storage(v, &out, (VkDeviceSize)rows * 4, "out")) {
    printf("FAIL: buffers: %s\n", v->why);
    failures++;
    goto done;
  }
  for (uint32_t c = 0; c < cols; c++) {
    float f = uniform();
    ((float *)x.bytes)[c] = f;
    xv[c] = f;
  }
  for (uint32_t r = 0; r < rows; r++) {
    make_row(type, cols, (uint8_t *)w.bytes + rb * r, values);
    double s = 0;
    for (uint32_t c = 0; c < cols; c++) {
      s += values[c] * xv[c];
    }
    want[r] = s;
  }
  memset(out.bytes, 0xff, (size_t)rows * 4);   /* a row not written is a NaN */

  struct kvk_span ws = { w.buffer, 0, 0 }, xs = { x.buffer, 0, 0 }, os = { out.buffer, 0, 0 };
  if (!kvk_begin(v) || !llm_vk_mmv(g, as, ws, cols, rows, xs, os)) {
    printf("FAIL: recording the product\n");
    failures++;
    goto done;
  }
  kvk_compute_barrier(v);
  if (!kvk_submit(v)) {
    printf("FAIL: submitting: %s\n", v->why);
    failures++;
    goto done;
  }
  snprintf(what, sizeof(what), "product, %s%s, %u rows of %u",
           type == LLM_F32 ? "F32" : type == LLM_F16 ? "F16" : type == LLM_Q4_0 ? "Q4_0" :
           type == LLM_Q8_0 ? "Q8_0" : "Q6_K", as != type ? " read as another type" : "",
           rows, cols);
  verdict(what, nmse((const float *)out.bytes, want, rows), 1e-7, as != type);

done:
  kvk_buffer_free(v, &out);
  kvk_buffer_free(v, &x);
  kvk_buffer_free(v, &w);
  free(values);
  free(want);
  free(xv);
}

/* ---------------------------------------------------------------------- */
/* A layer's other steps                                                   */
/* ---------------------------------------------------------------------- */

/* Each step gets up to three inputs of floats and an output, all made
 * here; its reference is worked out in double from the same floats. In
 * the control each reference is the one a characteristic mistake would
 * give - the weight left out, the mean not taken away, the bias dropped,
 * the other activation, positions off by one - which the step must then
 * miss by more than its bound. */
struct bufs {
  struct kvk_buffer in[3], out;
  struct kvk_span s[3], o;
};

static int bufs_make(struct kvk *v, struct bufs *b, size_t n0, size_t n1, size_t n2, size_t out_bytes)
{
  size_t n[3] = { n0, n1, n2 };
  memset(b, 0, sizeof(*b));
  for (int i = 0; i < 3; i++) {
    if (!kvk_buffer_make_storage(v, &b->in[i], n[i] ? n[i] * 4 : 4, "in")) {
      return 0;
    }
    b->s[i] = (struct kvk_span){ b->in[i].buffer, 0, 0 };
  }
  if (!kvk_buffer_make_storage(v, &b->out, out_bytes, "out")) {
    return 0;
  }
  b->o = (struct kvk_span){ b->out.buffer, 0, 0 };
  memset(b->out.bytes, 0xff, out_bytes);
  return 1;
}

static void bufs_free(struct kvk *v, struct bufs *b)
{
  kvk_buffer_free(v, &b->out);
  for (int i = 0; i < 3; i++) {
    kvk_buffer_free(v, &b->in[i]);
  }
}

static float *in_floats(struct bufs *b, int i) { return (float *)b->in[i].bytes; }

static void fill(float *x, size_t n, float scale)
{
  for (size_t i = 0; i < n; i++) {
    x[i] = uniform() * scale;
  }
}

/* Recorded already: submit and wait. */
static int run(struct kvk *v, const char *what)
{
  kvk_compute_barrier(v);
  if (!kvk_submit(v)) {
    printf("FAIL: %s: submitting: %s\n", what, v->why);
    failures++;
    return 0;
  }
  return 1;
}

static int begin(int recorded, const char *what)
{
  if (!recorded) {
    printf("FAIL: %s: not recorded\n", what);
    failures++;
  }
  return recorded;
}

static double gelu_ref(double x)
{
  return 0.5 * x * (1.0 + tanh(0.79788456080286535587989211986876 * x * (1.0 + 0.044715 * x * x)));
}

static double silu_ref(double x)
{
  return x / (1.0 + exp(-x));
}

/* Each step at one or two shapes the models have: phi-2's 2560 and Qwen
 * 2.5's 1536 and 8960, a row count past 512 (rows go down as well as
 * across), and an odd count of floats no workgroup divides. */
static void steps(struct llm_vk *g, int control)
{
  struct kvk *v = g->v;
  struct bufs b;
  char what[96];

  /* Norms. */
  static const struct { uint32_t cols, rows; } NORMS[] = { { 2560, 7 }, { 1536, 600 }, { 37, 3 } };
  for (size_t t = 0; t < sizeof(NORMS) / sizeof(NORMS[0]); t++) {
    uint32_t cols = NORMS[t].cols, rows = NORMS[t].rows;
    size_t n = (size_t)cols * rows;
    double *want = malloc(sizeof(double) * n);
    for (int rms = 0; rms < 2; rms++) {
      if (!bufs_make(v, &b, n, cols, 0, n * 4)) {
        printf("FAIL: buffers: %s\n", v->why);
        failures++;
        break;
      }
      float *x = in_floats(&b, 0), *w = in_floats(&b, 1);
      fill(x, n, 3.0f);
      fill(w, cols, 1.0f);
      const float eps = 1e-5f;
      for (uint32_t r = 0; r < rows; r++) {
        const float *xr = x + (size_t)r * cols;
        double mean = 0, sq = 0;
        for (uint32_t c = 0; c < cols; c++) {
          mean += xr[c];
        }
        mean /= cols;
        if (rms || control) {
          mean = 0;   /* RMS takes none away; the norm's control forgets to */
        }
        for (uint32_t c = 0; c < cols; c++) {
          sq += (xr[c] - mean) * (xr[c] - mean);
        }
        double inv = 1.0 / sqrt(sq / cols + eps);
        for (uint32_t c = 0; c < cols; c++) {
          double y = (xr[c] - mean) * inv;
          want[(size_t)r * cols + c] = rms && !control ? y * w[c] : y;
        }
      }
      snprintf(what, sizeof(what), "%s, %u rows of %u", rms ? "RMS norm and weight" : "norm", rows, cols);
      if (kvk_begin(v) &&
          begin(rms ? llm_vk_rms_norm(g, b.s[0], b.s[1], b.o, cols, rows, eps)
                       : llm_vk_norm(g, b.s[0], b.o, cols, rows, eps), what) &&
          run(v, what)) {
        verdict(what, nmse((const float *)b.out.bytes, want, n), 1e-7, control);
      }
      bufs_free(v, &b);
    }
    free(want);
  }

  /* ADD and MUL, b a whole tensor and b one row repeated. */
  static const struct { uint32_t cols, rows, b_rows; } BIN[] = {
    { 2560, 9, 1 }, { 1536, 700, 1 }, { 8960, 3, 3 }, { 33, 5, 5 },
  };
  for (size_t t = 0; t < sizeof(BIN) / sizeof(BIN[0]); t++) {
    uint32_t cols = BIN[t].cols, rows = BIN[t].rows, b_rows = BIN[t].b_rows;
    size_t n = (size_t)cols * rows;
    double *want = malloc(sizeof(double) * n);
    for (int mul = 0; mul < 2; mul++) {
      /* b has room for every row, so the control's reading of it - row by
       * row, never repeated - stays inside. */
      if (!bufs_make(v, &b, n, n, 0, n * 4)) {
        printf("FAIL: buffers: %s\n", v->why);
        failures++;
        break;
      }
      float *a = in_floats(&b, 0), *bb = in_floats(&b, 1);
      fill(a, n, 2.0f);
      fill(bb, n, 2.0f);
      for (size_t i = 0; i < n; i++) {
        size_t j = b_rows == 1 && !control ? i % cols : i;
        want[i] = mul ? (double)a[i] * bb[j] : (double)a[i] + bb[j];
      }
      snprintf(what, sizeof(what), "%s, %u rows of %u, b %u row%s", mul ? "MUL" : "ADD", rows, cols,
               b_rows, b_rows == 1 ? "" : "s");
      if (b_rows != 1 && control) {
        /* A whole b is read as itself either way: its control is b as
         * one row, repeated. */
        for (size_t i = 0; i < n; i++) {
          want[i] = mul ? (double)a[i] * bb[i % cols] : (double)a[i] + bb[i % cols];
        }
      }
      if (kvk_begin(v) &&
          begin(mul ? llm_vk_mul(g, b.s[0], b.s[1], b.o, cols, rows, b_rows)
                       : llm_vk_add(g, b.s[0], b.s[1], b.o, cols, rows, b_rows), what) &&
          run(v, what)) {
        verdict(what, nmse((const float *)b.out.bytes, want, n), 1e-7, control);
      }
      bufs_free(v, &b);
    }
    free(want);
  }

  /* The steps over a run of floats: SCALE, GELU, SiLU, SwiGLU. */
  static const uint32_t FLAT[] = { 10240, 300001, 513 };
  for (size_t t = 0; t < sizeof(FLAT) / sizeof(FLAT[0]); t++) {
    uint32_t n = FLAT[t];
    double *want = malloc(sizeof(double) * n);
    for (int op = 0; op < 4; op++) {
      static const char *NAMES[] = { "SCALE", "GELU", "SiLU", "SwiGLU" };
      if (!bufs_make(v, &b, n, n, 0, (size_t)n * 4)) {
        printf("FAIL: buffers: %s\n", v->why);
        failures++;
        break;
      }
      float *x = in_floats(&b, 0), *y = in_floats(&b, 1);
      fill(x, n, 6.0f);
      fill(y, n, 2.0f);
      const float s = 0.125f, bias = 0.5f;
      for (uint32_t i = 0; i < n; i++) {
        switch (op) {
        case 0: want[i] = (double)x[i] * s + (control ? 0 : bias); break;
        case 1: want[i] = control ? silu_ref(x[i]) : gelu_ref(x[i]); break;
        case 2: want[i] = control ? gelu_ref(x[i]) : silu_ref(x[i]); break;
        default: want[i] = control ? silu_ref(y[i]) * x[i] : silu_ref(x[i]) * y[i]; break;
        }
      }
      int recorded = 0;
      if (kvk_begin(v)) {
        switch (op) {
        case 0: recorded = llm_vk_scale(g, b.s[0], b.o, n, s, bias); break;
        case 1: recorded = llm_vk_gelu(g, b.s[0], b.o, n); break;
        case 2: recorded = llm_vk_silu(g, b.s[0], b.o, n); break;
        default: recorded = llm_vk_swiglu(g, b.s[0], b.s[1], b.o, n); break;
        }
      }
      snprintf(what, sizeof(what), "%s, %u floats", NAMES[op], n);
      if (begin(recorded, what) && run(v, what)) {
        verdict(what, nmse((const float *)b.out.bytes, want, n), 1e-7, control);
      }
      bufs_free(v, &b);
    }
    free(want);
  }

  /* Softmax: a token's scores over its context, and a batch's rows. */
  static const struct { uint32_t cols, rows; } SOFT[] = { { 1, 32 }, { 517, 16 }, { 4096, 600 } };
  for (size_t t = 0; t < sizeof(SOFT) / sizeof(SOFT[0]); t++) {
    uint32_t cols = SOFT[t].cols, rows = SOFT[t].rows;
    size_t n = (size_t)cols * rows;
    double *want = malloc(sizeof(double) * n);
    if (!bufs_make(v, &b, n, 0, 0, n * 4)) {
      printf("FAIL: buffers: %s\n", v->why);
      failures++;
      free(want);
      continue;
    }
    float *x = in_floats(&b, 0);
    fill(x, n, 8.0f);
    const float scale = 0.0883883476f;   /* 1/sqrt(128) */
    for (uint32_t r = 0; r < rows; r++) {
      const float *xr = x + (size_t)r * cols;
      double sc = control ? 1.0 : scale, top = -INFINITY, sum = 0;
      for (uint32_t c = 0; c < cols; c++) {
        top = fmax(top, xr[c] * sc);
      }
      for (uint32_t c = 0; c < cols; c++) {
        sum += exp(xr[c] * sc - top);
      }
      for (uint32_t c = 0; c < cols; c++) {
        want[(size_t)r * cols + c] = exp(xr[c] * sc - top) / sum;
      }
    }
    snprintf(what, sizeof(what), "softmax, %u rows of %u", rows, cols);
    /* One column is always 1, scaled or not: its control is a 0 in the
     * reference. */
    if (cols == 1 && control) {
      for (size_t i = 0; i < n; i++) {
        want[i] = 0;
      }
    }
    if (kvk_begin(v) && begin(llm_vk_soft_max(g, b.s[0], b.o, cols, rows, scale), what) &&
        run(v, what)) {
      verdict(what, nmse((const float *)b.out.bytes, want, n), 1e-7, control);
    }
    bufs_free(v, &b);
    free(want);
  }

  /* RoPE: Qwen 2.5 1.5B's heads (128, all of it turned), and phi-2's
   * (80, the first 32 turned and the rest copied), for one token and a
   * batch of them at far positions. */
  static const struct { uint32_t head, heads, tokens, n_rot; float base; uint32_t pos0; } ROPE[] = {
    { 128, 12, 1, 128, 1000000.0f, 37 }, { 128, 2, 9, 128, 1000000.0f, 4000 },
    { 80, 32, 5, 32, 10000.0f, 0 },
  };
  for (size_t t = 0; t < sizeof(ROPE) / sizeof(ROPE[0]); t++) {
    uint32_t head = ROPE[t].head, heads = ROPE[t].heads, tokens = ROPE[t].tokens, n_rot = ROPE[t].n_rot;
    float base = ROPE[t].base;
    size_t n = (size_t)head * heads * tokens;
    double *want = malloc(sizeof(double) * n);
    if (!bufs_make(v, &b, n, tokens, 0, n * 4)) {
      printf("FAIL: buffers: %s\n", v->why);
      failures++;
      free(want);
      continue;
    }
    float *x = in_floats(&b, 0);
    int32_t *pos = (int32_t *)b.in[1].bytes;
    fill(x, n, 1.0f);
    for (uint32_t k = 0; k < tokens; k++) {
      pos[k] = (int32_t)(ROPE[t].pos0 + 3 * k);
    }
    for (uint32_t k = 0; k < tokens; k++) {
      for (uint32_t h = 0; h < heads; h++) {
        const float *xh = x + ((size_t)k * heads + h) * head;
        double *wh = want + ((size_t)k * heads + h) * head;
        for (uint32_t i = 0; i < head; i++) {
          wh[i] = xh[i];
        }
        double p = pos[k] + (control ? 1 : 0);
        for (uint32_t i = 0; i < n_rot / 2; i++) {
          double theta = p * pow(base, -2.0 * i / n_rot);
          double c = cos(theta), s = sin(theta);
          wh[i] = xh[i] * c - xh[i + n_rot / 2] * s;
          wh[i + n_rot / 2] = xh[i] * s + xh[i + n_rot / 2] * c;
        }
      }
    }
    snprintf(what, sizeof(what), "RoPE, %u tokens of %u heads of %u, %u turned", tokens, heads, head,
             n_rot);
    if (kvk_begin(v) &&
        begin(llm_vk_rope_neox(g, b.s[0], b.s[1], b.o, head, heads, tokens, n_rot, base), what) &&
        run(v, what)) {
      /* The angle is a float's: `pos * pow(theta_scale, i)` on the GPU, at
       * a position of four thousand radians, is good to about 1e-4 of a
       * radian - test-backend-ops' own bound for RoPE, 1e-7 of NMSE, holds
       * it. */
      verdict(what, nmse((const float *)b.out.bytes, want, n), 1e-7, control);
    }
    bufs_free(v, &b);
    free(want);
  }

  /* Attention's two products over an F16 cache (`mul_mat_vec_nc`), at
   * the models' heads: the keys' scores, a position a row KV wide, and the
   * values' sum over the transposed cache, n_ctx a row. Qwen 2.5 0.5B's 14
   * heads on 2 (a divisor of 7), phi-2's 32 of 80, 1.5B's 12 on 2 of 128,
   * at positions that do and do not fill vec4s. The control reads each
   * head's query as the next head's. */
  static const struct { uint32_t D, H, HKV, n, n_ctx; } NC[] = {
    { 64, 14, 2, 37, 512 }, { 80, 32, 32, 300, 512 }, { 128, 12, 2, 4, 512 }, { 64, 14, 2, 512, 512 },
  };
  for (size_t t = 0; t < sizeof(NC) / sizeof(NC[0]); t++) {
    const uint32_t D = NC[t].D, H = NC[t].H, KV = NC[t].HKV * D, n = NC[t].n, n_ctx = NC[t].n_ctx;
    const uint32_t div = H / NC[t].HKV;
    for (int values = 0; values < 2; values++) {
      /* keys: cache [n_ctx][KV]; values: transposed, [KV][n_ctx]. */
      size_t cache = (size_t)n_ctx * KV, bn = values ? (size_t)H * n : (size_t)H * D;
      size_t outn = values ? (size_t)H * D : (size_t)H * n;
      double *want = malloc(sizeof(double) * outn);
      struct kvk_buffer a = { 0 };
      if (!bufs_make(v, &b, bn, 0, 0, outn * 4) || !kvk_buffer_make_storage(v, &a, cache * 2, "cache")) {
        printf("FAIL: buffers: %s\n", v->why);
        failures++;
        free(want);
        continue;
      }
      uint16_t *ac = (uint16_t *)a.bytes;
      for (size_t i = 0; i < cache; i++) {
        ac[i] = half(uniform());
      }
      float *bv = in_floats(&b, 0);
      fill(bv, bn, 1.0f);
      uint32_t cols = values ? n : D, rows = values ? D : n;
      uint32_t a_row = values ? n_ctx : KV, a_chan = values ? D * n_ctx : D, b_chan = values ? n : D;
      for (uint32_t c = 0; c < H; c++) {
        uint32_t cb = control ? (c + 1) % H : c;
        for (uint32_t r = 0; r < rows; r++) {
          double sum = 0;
          for (uint32_t k = 0; k < cols; k++) {
            sum += (double)llm_f16_to_f32(ac[(size_t)(c / div) * a_chan + (size_t)r * a_row + k]) *
                   bv[(size_t)cb * b_chan + k];
          }
          want[(size_t)c * rows + r] = sum;
        }
      }
      snprintf(what, sizeof(what), "attention's %s, %u heads on %u of %u, %u positions",
               values ? "values' sum" : "keys' scores", H, NC[t].HKV, D, n);
      if (kvk_begin(v) &&
          begin(llm_vk_mmv_nc(g, (struct kvk_span){ a.buffer, 0, 0 }, b.s[0], b.o, cols, rows, a_row, a_chan,
                              b_chan, div, H), what) &&
          run(v, what)) {
        verdict(what, nmse((const float *)b.out.bytes, want, outn), 1e-7, control);
      }
      kvk_buffer_free(v, &a);
      bufs_free(v, &b);
      free(want);
    }
  }

  /* A value down its column of the transposed cache: KV floats to F16,
   * n_ctx apart, at position `at`. Held bit for bit, the rest of the cache
   * untouched; the control expects it one position on. */
  {
    const uint32_t KV = 128, n_ctx = 512, at = 301;
    struct kvk_buffer a = { 0 };
    if (bufs_make(v, &b, KV, 0, 0, 4) && kvk_buffer_make_storage(v, &a, (size_t)KV * n_ctx * 2, "cache")) {
      float *x = in_floats(&b, 0);
      fill(x, KV, 4.0f);
      memset(a.bytes, 0, (size_t)KV * n_ctx * 2);
      snprintf(what, sizeof(what), "a value down the transposed cache, %u floats %u apart, at %u", KV, n_ctx, at);
      if (kvk_begin(v) &&
          begin(llm_vk_cpy_f16_strided(g, b.s[0], (struct kvk_span){ a.buffer, 0, 0 }, KV, n_ctx, at), what) &&
          run(v, what)) {
        const uint16_t *c = (const uint16_t *)a.bytes;
        uint32_t differ = 0;
        uint32_t where = control ? at + 1 : at;
        for (size_t i = 0; i < (size_t)KV * n_ctx; i++) {
          uint16_t want = i % n_ctx == where ? llm_f32_to_f16(x[i / n_ctx]) : 0;
          differ += c[i] != want;
        }
        int ok = control ? differ > 0 : differ == 0;
        printf("%s: %s, %u differ (%s)\n", ok ? "PASS" : "FAIL", what, differ, control ? "some must" : "none may");
        failures += !ok;
      }
    } else {
      printf("FAIL: buffers: %s\n", v->why);
      failures++;
    }
    kvk_buffer_free(v, &a);
    bufs_free(v, &b);
  }

  /* The copies: to F16 exactly as `llm_f32_to_f16` rounds, and to F32
   * exactly; held bit for bit. The control is the F16 one held to the
   * floats before rounding. */
  for (int half_out = 0; half_out < 2; half_out++) {
    const uint32_t n = 70001;
    if (!bufs_make(v, &b, n, 0, 0, (size_t)n * 4)) {
      printf("FAIL: buffers: %s\n", v->why);
      failures++;
      continue;
    }
    float *x = in_floats(&b, 0);
    fill(x, n, 100.0f);
    snprintf(what, sizeof(what), "copy to %s, %u floats", half_out ? "F16" : "F32", n);
    if (kvk_begin(v) &&
        begin(half_out ? llm_vk_cpy_f16(g, b.s[0], b.o, n) : llm_vk_cpy_f32(g, b.s[0], b.o, n), what) &&
        run(v, what)) {
      uint32_t differ = 0;
      for (uint32_t i = 0; i < n; i++) {
        if (half_out) {
          uint16_t got = ((const uint16_t *)b.out.bytes)[i];
          uint16_t want = control ? 0 : llm_f32_to_f16(x[i]);
          differ += control ? llm_f16_to_f32(got) != x[i] : got != want;
        } else {
          differ += ((const float *)b.out.bytes)[i] != x[i];
        }
      }
      if (control && !half_out) {
        differ = 1;   /* nothing to get wrong in a float's copy: the F16 one is the control */
      }
      int ok = control ? differ > 0 : differ == 0;
      printf("%s: %s, %u differ (%s)\n", ok ? "PASS" : "FAIL", what, differ,
             control ? "some must" : "none may");
      failures += !ok;
    }
    bufs_free(v, &b);
  }
}

/* ---------------------------------------------------------------------- */
/* A model, a token at a time (V4)                                         */
/* ---------------------------------------------------------------------- */

static void *alloc(void *ctx, size_t bytes)
{
  (void)ctx;
  return calloc(1, bytes ? bytes : 1);
}

#define ANSWER 32

/* The question and then ANSWER tokens, the likeliest after each: on the
 * processor, keeping each step's logits. */
static int answer_cpu(struct llm_model *m, const int32_t *q, int nq, int32_t *out, float *logits)
{
  llm_reset(m);
  for (int i = 0; i < nq; i++) {
    if (llm_step(m, q[i]) != 0) {
      return -1;
    }
  }
  for (int i = 0; i < ANSWER; i++) {
    memcpy(logits + (size_t)i * m->n_vocab, m->logits, (size_t)m->n_vocab * 4);
    out[i] = llm_greedy(m);
    if (i + 1 < ANSWER && llm_step(m, out[i]) != 0) {
      return -1;
    }
  }
  return 0;
}

static double nmse_f(const float *got, const float *want, size_t n)
{
  double err = 0, norm = 0;
  for (size_t i = 0; i < n; i++) {
    double d = (double)got[i] - want[i];
    err += d * d;
    norm += (double)want[i] * want[i];
  }
  return norm > 0 ? err / norm : err;
}

/* The model in `path` on the GPU, held to the processor at every step of
 * an answer, the GPU given the processor's tokens: every step's logits
 * within 1e-3 of NMSE of the processor's (its products take the input as
 * Q8_0 blocks, the GPU's as floats; `test-backend-ops` holds one product
 * to 5e-4, and a model is dozens of them), and the GPU's own likeliest
 * token counted beside the processor's. With `whole`, the GPU's answer
 * must also be the processor's, all ANSWER tokens: a real model, whose
 * likeliest token is clear; a random one's are near ties. The control
 * gives the GPU the question less its last token, as a token dropped on
 * the way would, which must break what is held. (A cache started a
 * position on is not one: RoPE sees only the distances between tokens,
 * and phi-2's answer barely moved.) */
static int model(struct llm_vk *g, const char *path, int whole_answer, int control)
{
  int fd = open(path, O_RDONLY);
  struct stat st;
  if (fd < 0 || fstat(fd, &st) != 0) {
    printf("FAIL: %s: cannot be read\n", path);
    return 1;
  }
  void *file = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (file == MAP_FAILED) {
    printf("FAIL: %s: cannot be mapped\n", path);
    return 1;
  }

  static struct llm_model m;
  struct llm_vk_model vm;
  char why[200];
  memset(&m, 0, sizeof(m));
  if (llm_open(&m, file, (size_t)st.st_size, 512, alloc, NULL, why, sizeof(why)) != 0) {
    printf("FAIL: %s: %s\n", path, why);
    return 1;
  }
  if (!llm_vk_model_open(&vm, g, &m)) {
    printf("FAIL: %s on the GPU: %s\n", path, vm.why);
    return 1;
  }

  const char *text = m.kind == LLM_ARCH_QWEN2
                         ? "<|im_start|>user\nWhat is the capital of France?<|im_end|>\n<|im_start|>assistant\n"
                         : "Instruct: What is the capital of France?\nOutput:";
  static int32_t q[256];
  int nq = llm_tokenize(&m, text, strlen(text), 1, q, 256);
  int32_t cpu[ANSWER], gpu[ANSWER];
  float *logits = malloc((size_t)m.n_vocab * 4 * ANSWER);
  int bad = 1;

  if (nq <= 0 || !logits || answer_cpu(&m, q, nq, cpu, logits) != 0) {
    printf("FAIL: %s: the processor's answer\n", path);
    goto done;
  }

  llm_reset(&m);
  int r = 0;
  for (int i = 0; r == 0 && i < nq - (control ? 1 : 0); i++) {
    r = llm_vk_step(&vm, q[i]);
  }
  double worst = 0;
  int agree = 0;
  for (int i = 0; r == 0 && i < ANSWER; i++) {
    double e = nmse_f(m.logits, logits + (size_t)i * m.n_vocab, m.n_vocab);
    worst = e > worst ? e : worst;
    if (getenv("LLMVK_STEPS")) {
      printf("  step %2d: nmse %.3g\n", i, e);
    }
    gpu[i] = llm_greedy(&m);
    agree += gpu[i] == cpu[i];
    if (gpu[i] != cpu[i]) {
      const float *c = logits + (size_t)i * m.n_vocab;
      printf("  step %d: the processor's %d (%.4f, the GPU's %.4f), the GPU's %d (%.4f, the GPU's %.4f)\n", i,
             cpu[i], c[cpu[i]], m.logits[cpu[i]], gpu[i], c[gpu[i]], m.logits[gpu[i]]);
    }
    if (i + 1 < ANSWER) {
      r = llm_vk_step(&vm, cpu[i]);
    }
  }
  if (r != 0) {
    printf("FAIL: %s: the GPU's answer: %s\n", path, vm.why);
    goto done;
  }

  /* And the GPU's own answer, where the whole of it is held. */
  int own = whole_answer && !control ? 0 : ANSWER;
  if (whole_answer && !control && agree == ANSWER) {
    llm_reset(&m);
    for (int i = 0; r == 0 && i < nq; i++) {
      r = llm_vk_step(&vm, q[i]);
    }
    for (int i = 0; r == 0 && i < ANSWER; i++) {
      gpu[i] = llm_greedy(&m);
      if (i + 1 < ANSWER) {
        r = llm_vk_step(&vm, gpu[i]);
      }
    }
    own = 0;
    while (r == 0 && own < ANSWER && gpu[own] == cpu[own]) {
      own++;
    }
  }

  char said[ANSWER * 24] = "", one[64];
  for (int i = 0; i < ANSWER; i++) {
    int n = llm_token_text(&m, cpu[i], one, sizeof(one) - 1);
    one[n > 0 ? n : 0] = 0;
    strncat(said, one, sizeof(said) - strlen(said) - 1);
  }
  /* A real model is held to its answer, as decision 1 has it; its logits
   * are reported. A tiny one's answer is near ties, and its logits are
   * held instead. */
  int held = whole_answer ? agree == ANSWER && own == ANSWER : worst <= 1e-3;
  int ok = control ? !held : held;
  printf("%s: %s, a %d-token question, %d answer tokens: every step's logits within %.3g of NMSE%s, "
         "the likeliest the processor's at %d of %d steps",
         ok ? "PASS" : "FAIL", path, nq, ANSWER, worst, whole_answer ? "" : " (held to 1e-3)", agree, ANSWER);
  if (whole_answer && !control) {
    if (agree == ANSWER) {
      printf(", the GPU's own answer the processor's for %d of %d tokens", own, ANSWER);
    } else {
      printf(", so the GPU's own answer is not the processor's");
    }
  }
  if (control) {
    printf(" - the control, which must not hold");
  }
  printf("\n      \"%s\"\n", said);
  bad = !ok;

done:
  free(logits);
  llm_vk_model_close(&vm);
  munmap(file, (size_t)st.st_size);
  return bad;
}

int main(int argc, char **argv)
{
  int control = 0, whole_answer = 0;
  const char *model_path = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--control") == 0) {
      control = 1;
    } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
      model_path = argv[++i];
    } else if (strcmp(argv[i], "--whole") == 0) {
      whole_answer = 1;
    }
  }
  struct kvk v;
  struct llm_vk g;

  if (!kvk_open(&v, "test_llmvk", KVK_COMPUTE)) {
    printf("FAIL: no Vulkan device: %s %s\n", v.device_name, v.why);
    return 1;
  }
  if (!llm_vk_open(&g, &v)) {
    printf("FAIL: %s\n", g.why);
    return 1;
  }
  printf("device %s, %u lanes a subgroup\n", v.device_name, v.subgroup_size);
  if (model_path) {
    int bad = model(&g, model_path, whole_answer, control);
    llm_vk_close(&g);
    kvk_close(&v);
    return bad;
  }

  /* The shapes the models have, and odd ones: a row count no workgroup's
   * rows divide, and Qwen's output matrix's 151,936 rows, more than a
   * dispatch has workgroups across. */
  static const struct { uint32_t type, cols, rows; } CASES[] = {
    { LLM_F32, 256, 37 },       { LLM_F16, 2560, 65 },
    { LLM_Q4_0, 2560, 2560 },   { LLM_Q4_0, 1536, 8961 },  { LLM_Q4_0, 64, 3 },
    { LLM_Q8_0, 2560, 513 },    { LLM_Q8_0, 8960, 129 },
    { LLM_Q6_K, 1536, 151936 }, { LLM_Q6_K, 256, 7 },
  };
  /* Each type's blocks read as the next one's. */
  static const uint32_t OTHER[] = { [LLM_Q4_0] = LLM_Q8_0, [LLM_Q8_0] = LLM_Q4_0,
                                    [LLM_F16] = LLM_F32, [LLM_F32] = LLM_F16,
                                    [LLM_Q6_K] = LLM_Q4_0 };

  for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
    uint32_t t = CASES[i].type;
    product(&g, t, control ? OTHER[t] : t, CASES[i].cols, CASES[i].rows);
  }
  steps(&g, control);

  llm_vk_close(&g);
  kvk_close(&v);
  printf("%s: %d failed\n", failures ? "FAIL" : "PASS", failures);
  return failures != 0;
}
