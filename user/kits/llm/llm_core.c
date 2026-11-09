/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The LLM Kit's core: GGUF, GPT-2's tokenizer, and phi-2's forward pass
 * (`llm_core.h`, `docs/xavier.md` X1).
 *
 * **Written to llama.cpp's arithmetic, not only its algorithm**, because
 * the host test holds this to llama.cpp token for token, and a model's
 * next word turns on its last bits more often than one would think. So
 * where llama.cpp rounds, this rounds the same way: the input to every
 * product is quantised to Q8_0 first and the product is in integers, as
 * ggml's `vec_dot_q8_0_q8_0` and `vec_dot_q4_0_q8_0`; the attention cache
 * is F16, and so are the query and the probabilities as they meet it; GELU
 * goes through F16, as ggml's table does; RoPE's angles are a running
 * product in `float`, as `ggml_rope_cache_init` makes them; LayerNorm sums
 * in `double`. Each is marked where it is done.
 *
 * Scalar, one thread, step X1. The vector unit is X2 and the threads X5,
 * and this loop stays as the reference both are held to.
 */

#include "llm_core.h"

#include <math.h>
#include <string.h>

/*
 * **A trace of every stage**, compiled in only when asked (`-DLLM_TRACE`,
 * the host test's `--trace`): each named as llama.cpp's graph names it,
 * so the two can be laid side by side when they disagree - which is how a
 * kernel on the GPU will be held to this loop too.
 */
#ifdef LLM_TRACE
void llm_trace(const char *name, uint32_t layer, const float *x, uint32_t n);
#define TRACE(name, layer, x, n) llm_trace(name, layer, x, n)
#else
#define TRACE(name, layer, x, n) ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* Half precision                                                      */
/* ------------------------------------------------------------------ */

static uint32_t f32_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float bits_f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

uint16_t llm_f32_to_f16(float f)
{
  uint32_t x = f32_bits(f);
  uint32_t sign = (x >> 16) & 0x8000;
  uint32_t mant = x & 0x007fffff;
  int32_t exp = (int32_t)((x >> 23) & 0xff);

  if (exp == 0xff)                                   /* inf, nan */
    return (uint16_t)(sign | 0x7c00 | (mant ? 0x200 : 0));

  exp = exp - 127 + 15;

  if (exp >= 0x1f) return (uint16_t)(sign | 0x7c00); /* too large: inf */

  if (exp <= 0) {                                    /* a subnormal half */
    if (exp < -10) return (uint16_t)sign;
    mant |= 0x00800000;
    uint32_t shift = (uint32_t)(14 - exp);
    uint32_t half = mant >> shift;
    uint32_t rest = mant & ((1u << shift) - 1);
    uint32_t mid = 1u << (shift - 1);
    if (rest > mid || (rest == mid && (half & 1))) half++;
    return (uint16_t)(sign | half);
  }

  uint32_t half = ((uint32_t)exp << 10) | (mant >> 13);
  uint32_t rest = mant & 0x1fff;
  if (rest > 0x1000 || (rest == 0x1000 && (half & 1))) half++;   /* may carry into exp: right */
  return (uint16_t)(sign | half);
}

/* ------------------------------------------------------------------ */
/* Reading the file, held to its size                                  */
/* ------------------------------------------------------------------ */

struct rd {
  const uint8_t *p, *end;
  int bad;
};

static const uint8_t *take(struct rd *r, uint64_t n)
{
  if (r->bad || n > (uint64_t)(r->end - r->p)) { r->bad = 1; return NULL; }
  const uint8_t *at = r->p;
  r->p += n;
  return at;
}

static uint32_t rd_u32(struct rd *r)
{
  const uint8_t *b = take(r, 4);
  return b ? (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24 : 0;
}

static uint64_t rd_u64(struct rd *r)
{
  uint64_t lo = rd_u32(r);
  return lo | (uint64_t)rd_u32(r) << 32;
}

static struct llm_str rd_str(struct rd *r)
{
  struct llm_str s = { 0, 0 };
  uint64_t n = rd_u64(r);
  if (n > 0xffffffffu) { r->bad = 1; return s; }
  s.s = (const char *)take(r, n);
  s.n = s.s ? (uint32_t)n : 0;
  return s;
}

/* GGUF's value types. */
enum { G_U8, G_I8, G_U16, G_I16, G_U32, G_I32, G_F32, G_BOOL, G_STR, G_ARR, G_U64, G_I64, G_F64 };

static uint64_t scalar_size(uint32_t t)
{
  switch (t) {
    case G_U8: case G_I8: case G_BOOL: return 1;
    case G_U16: case G_I16: return 2;
    case G_U32: case G_I32: case G_F32: return 4;
    case G_U64: case G_I64: case G_F64: return 8;
    default: return 0;
  }
}

/* Past one value of type `t`. Arrays of arrays are refused: no model has one. */
static void skip_value(struct rd *r, uint32_t t)
{
  if (t == G_STR) { rd_str(r); return; }

  if (t == G_ARR) {
    uint32_t et = rd_u32(r);
    uint64_t n = rd_u64(r);

    if (et == G_STR) {
      for (uint64_t i = 0; i < n && !r->bad; i++) rd_str(r);
    } else if (scalar_size(et) && n <= (uint64_t)(r->end - r->p) / scalar_size(et)) {
      take(r, n * scalar_size(et));
    } else {
      r->bad = 1;
    }
    return;
  }

  if (!scalar_size(t)) { r->bad = 1; return; }
  take(r, scalar_size(t));
}

struct meta {
  struct llm_str key;
  uint32_t type;
  const uint8_t *value;
};

static int str_is(struct llm_str s, const char *c)
{
  size_t n = strlen(c);
  return s.n == n && memcmp(s.s, c, n) == 0;
}

static const struct meta *find_meta(const struct llm_model *m, const char *key)
{
  const struct meta *kv = m->meta;
  for (uint32_t i = 0; i < m->n_meta; i++)
    if (str_is(kv[i].key, key)) return &kv[i];
  return NULL;
}

static double number_at(uint32_t t, const uint8_t *v)
{
  uint64_t u = 0;
  uint64_t n = scalar_size(t);
  for (uint64_t i = 0; i < n; i++) u |= (uint64_t)v[i] << (8 * i);

  switch (t) {
    case G_U8: case G_BOOL: return (double)(uint8_t)u;
    case G_I8:  return (double)(int8_t)u;
    case G_U16: return (double)(uint16_t)u;
    case G_I16: return (double)(int16_t)u;
    case G_U32: return (double)(uint32_t)u;
    case G_I32: return (double)(int32_t)u;
    case G_U64: return (double)u;
    case G_I64: return (double)(int64_t)u;
    case G_F32: return (double)bits_f32((uint32_t)u);
    case G_F64: { double d; memcpy(&d, &u, 8); return d; }
    default: return 0;
  }
}

double llm_meta_number(const struct llm_model *m, const char *key, double fallback)
{
  const struct meta *kv = find_meta(m, key);
  return (kv && scalar_size(kv->type)) ? number_at(kv->type, kv->value) : fallback;
}

static struct llm_str meta_str(const struct llm_model *m, const char *key)
{
  struct llm_str s = { 0, 0 };
  const struct meta *kv = find_meta(m, key);
  if (kv && kv->type == G_STR) {
    struct rd r = { kv->value, m->file + m->size, 0 };
    s = rd_str(&r);
  }
  return s;
}

/* A key of the architecture's own: `phi2.block_count`. */
static double arch_number(const struct llm_model *m, const char *what, double fallback)
{
  char key[96];
  if (m->arch.n + 1 + strlen(what) + 1 > sizeof key) return fallback;
  memcpy(key, m->arch.s, m->arch.n);
  key[m->arch.n] = '.';
  strcpy(key + m->arch.n + 1, what);
  return llm_meta_number(m, key, fallback);
}

/* An array's elements: where they start, their type and how many. */
static int meta_array(const struct llm_model *m, const char *key, uint32_t *type, uint64_t *n,
                      struct rd *at)
{
  const struct meta *kv = find_meta(m, key);
  if (!kv || kv->type != G_ARR) return 0;
  at->p = kv->value;
  at->end = m->file + m->size;
  at->bad = 0;
  *type = rd_u32(at);
  *n = rd_u64(at);
  return !at->bad;
}

/* ------------------------------------------------------------------ */
/* Opening                                                             */
/* ------------------------------------------------------------------ */

static void say(char *why, size_t room, const char *what)
{
  if (!why || !room) return;
  size_t n = strlen(what);
  if (n >= room) n = room - 1;
  memcpy(why, what, n);
  why[n] = 0;
}

static uint64_t tensor_bytes(uint32_t type, uint64_t elements, int *ok)
{
  *ok = 1;
  switch (type) {
    case LLM_F32: return elements * 4;
    case LLM_F16: return elements * 2;
    case LLM_Q8_0: if (elements % LLM_QK) break; return elements / LLM_QK * 34;
    case LLM_Q4_0: if (elements % LLM_QK) break; return elements / LLM_QK * 18;
  }
  *ok = 0;
  return 0;
}

static const struct llm_tensor *tensor(const struct llm_model *m, const char *name)
{
  for (uint32_t i = 0; i < m->n_tensors; i++)
    if (str_is(m->tensors[i].name, name)) return &m->tensors[i];
  return NULL;
}

static const struct llm_tensor *layer_tensor(const struct llm_model *m, uint32_t layer,
                                             const char *what)
{
  char name[96];
  int n = 0;
  char digits[12];
  uint32_t v = layer;
  int d = 0;

  do { digits[d++] = (char)('0' + v % 10); v /= 10; } while (v);
  memcpy(name, "blk.", 4); n = 4;
  while (d) name[n++] = digits[--d];
  name[n++] = '.';
  if (n + strlen(what) + 1 > sizeof name) return NULL;
  strcpy(name + n, what);
  return tensor(m, name);
}

/* A matrix of `rows` rows of `cols`, in a type this multiplies. */
static int is_matrix(const struct llm_tensor *t, uint64_t cols, uint64_t rows)
{
  return t && t->n_dims == 2 && t->ne[0] == cols && t->ne[1] == rows
         && (t->type == LLM_F32 || t->type == LLM_F16 || t->type == LLM_Q8_0
             || t->type == LLM_Q4_0);
}

static int is_vector(const struct llm_tensor *t, uint64_t n)
{
  return t && t->n_dims == 1 && t->ne[0] == n && t->type == LLM_F32;
}

static uint32_t hash_bytes(const char *s, uint32_t n)
{
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
  return h;
}

static uint32_t pow2_above(uint32_t n)
{
  uint32_t p = 1;
  while (p < n * 2) p <<= 1;
  return p;
}

static int32_t token_id(const struct llm_model *m, const char *s, uint32_t n)
{
  uint32_t mask = m->token_hash_n - 1;
  for (uint32_t h = hash_bytes(s, n) & mask;; h = (h + 1) & mask) {
    uint32_t v = m->token_hash[h];
    if (!v) return -1;
    struct llm_str t = m->tokens[v - 1];
    if (t.n == n && memcmp(t.s, s, n) == 0) return (int32_t)(v - 1);
  }
}

static int32_t merge_rank(const struct llm_model *m, const char *s, uint32_t n)
{
  uint32_t mask = m->merge_hash_n - 1;
  for (uint32_t h = hash_bytes(s, n) & mask;; h = (h + 1) & mask) {
    uint32_t v = m->merge_hash[h];
    if (!v) return -1;
    struct llm_str t = m->merges[v - 1];
    if (t.n == n && memcmp(t.s, s, n) == 0) return (int32_t)(v - 1);
  }
}

static int open_tokenizer(struct llm_model *m, llm_alloc_fn alloc, void *ctx, char *why,
                          size_t room)
{
  struct llm_str model = meta_str(m, "tokenizer.ggml.model");
  uint32_t type;
  uint64_t n;
  struct rd r;

  if (!str_is(model, "gpt2")) {
    say(why, room, "its tokenizer is not GPT-2's byte-level BPE");
    return -1;
  }

  if (!meta_array(m, "tokenizer.ggml.tokens", &type, &n, &r) || type != G_STR
      || n != m->n_vocab) {
    say(why, room, "its token list is missing or not the vocabulary's size");
    return -1;
  }

  m->tokens = alloc(ctx, n * sizeof *m->tokens);
  m->token_type = alloc(ctx, n * sizeof *m->token_type);
  m->token_hash_n = pow2_above((uint32_t)n);
  m->token_hash = alloc(ctx, (size_t)m->token_hash_n * sizeof *m->token_hash);
  if (!m->tokens || !m->token_type || !m->token_hash) goto no_memory;

  for (uint32_t i = 0; i < n; i++) {
    m->tokens[i] = rd_str(&r);
    if (r.bad) { say(why, room, "a token runs past the file"); return -1; }

    /* Into the table, unless the same text is there already: the first
     * id wins, as llama.cpp's `token_to_id` keeps the first. */
    if (token_id(m, m->tokens[i].s, m->tokens[i].n) < 0) {
      uint32_t mask = m->token_hash_n - 1, h = hash_bytes(m->tokens[i].s, m->tokens[i].n) & mask;
      while (m->token_hash[h]) h = (h + 1) & mask;
      m->token_hash[h] = i + 1;
    }
    m->token_type[i] = 1;
  }

  if (meta_array(m, "tokenizer.ggml.token_type", &type, &n, &r) && n == m->n_vocab
      && (type == G_I32 || type == G_U32)) {
    for (uint32_t i = 0; i < n; i++) m->token_type[i] = (int32_t)rd_u32(&r);
  }

  if (!meta_array(m, "tokenizer.ggml.merges", &type, &n, &r) || type != G_STR
      || n > 0xfffffffeu) {
    say(why, room, "it has no BPE merges");
    return -1;
  }

  m->n_merges = (uint32_t)n;
  m->merges = alloc(ctx, n * sizeof *m->merges);
  m->merge_hash_n = pow2_above((uint32_t)n);
  m->merge_hash = alloc(ctx, (size_t)m->merge_hash_n * sizeof *m->merge_hash);
  if (!m->merges || !m->merge_hash) goto no_memory;

  for (uint32_t i = 0; i < n; i++) {
    m->merges[i] = rd_str(&r);
    if (r.bad) { say(why, room, "a merge runs past the file"); return -1; }
    if (merge_rank(m, m->merges[i].s, m->merges[i].n) < 0) {
      uint32_t mask = m->merge_hash_n - 1, h = hash_bytes(m->merges[i].s, m->merges[i].n) & mask;
      while (m->merge_hash[h]) h = (h + 1) & mask;
      m->merge_hash[h] = i + 1;
    }
  }

  /* The tokens matched in the text before BPE: user-defined ones always,
   * control ones when asked - longest first, as llama.cpp sorts them. */
  m->specials = alloc(ctx, m->n_vocab * sizeof *m->specials);
  if (!m->specials) goto no_memory;

  for (uint32_t i = 0; i < m->n_vocab; i++)
    if ((m->token_type[i] == 3 || m->token_type[i] == 4) && m->tokens[i].n)
      m->specials[m->n_specials++] = i;

  for (uint32_t i = 1; i < m->n_specials; i++) {           /* insertion, stable */
    uint32_t v = m->specials[i], j = i;
    while (j && m->tokens[m->specials[j - 1]].n < m->tokens[v].n) {
      m->specials[j] = m->specials[j - 1];
      j--;
    }
    m->specials[j] = v;
  }

  /* How the text is cut into words before BPE: Qwen2's own rule where the
   * file names it, GPT-2's otherwise (`llama-vocab.cpp`'s `pre`). */
  m->qwen2_split = str_is(meta_str(m, "tokenizer.ggml.pre"), "qwen2");

  m->bos = (int32_t)llm_meta_number(m, "tokenizer.ggml.bos_token_id", -1);
  m->eos = (int32_t)llm_meta_number(m, "tokenizer.ggml.eos_token_id", -1);
  return 0;

no_memory:
  say(why, room, "no memory for the tokenizer's tables");
  return -1;
}

static void gelu_table(void);

int llm_open(struct llm_model *m, const void *file, size_t size, uint32_t n_ctx,
             llm_alloc_fn alloc, void *ctx, char *why, size_t room)
{
  memset(m, 0, sizeof *m);
  m->file = file;
  m->size = size;
  gelu_table();

  struct rd r = { m->file, m->file + size, 0 };
  const uint8_t *magic = take(&r, 4);

  if (!magic || memcmp(magic, "GGUF", 4) != 0) {
    say(why, room, "not a GGUF file");
    return -1;
  }

  m->version = rd_u32(&r);
  if (m->version != 2 && m->version != 3) {
    say(why, room, "a GGUF version this does not read (2 and 3 are)");
    return -1;
  }

  uint64_t n_tensors = rd_u64(&r), n_meta = rd_u64(&r);

  if (r.bad || n_tensors > 1u << 20 || n_meta > 1u << 20) {
    say(why, room, "its header says more than a model has");
    return -1;
  }

  struct meta *kv = alloc(ctx, (size_t)n_meta * sizeof *kv + 1);
  m->tensors = alloc(ctx, (size_t)n_tensors * sizeof *m->tensors + 1);
  if (!kv || !m->tensors) { say(why, room, "no memory for its index"); return -1; }
  m->meta = kv;
  m->n_meta = (uint32_t)n_meta;
  m->n_tensors = (uint32_t)n_tensors;

  for (uint64_t i = 0; i < n_meta && !r.bad; i++) {
    kv[i].key = rd_str(&r);
    kv[i].type = rd_u32(&r);
    kv[i].value = r.p;
    skip_value(&r, kv[i].type);
  }

  for (uint64_t i = 0; i < n_tensors && !r.bad; i++) {
    struct llm_tensor *t = &m->tensors[i];
    t->name = rd_str(&r);
    t->n_dims = rd_u32(&r);
    if (t->n_dims == 0 || t->n_dims > LLM_MAX_DIMS) { r.bad = 1; break; }
    for (uint32_t d = 0; d < LLM_MAX_DIMS; d++) t->ne[d] = 1;
    for (uint32_t d = 0; d < t->n_dims; d++) t->ne[d] = rd_u64(&r);
    t->type = rd_u32(&r);
    t->bytes = rd_u64(&r);                 /* its offset, for now */
  }

  if (r.bad) {
    say(why, room, "its index runs past the end of the file");
    return -1;
  }

  uint64_t align = (uint64_t)llm_meta_number(m, "general.alignment", 32);
  if (align == 0 || (align & (align - 1))) { say(why, room, "a bad alignment"); return -1; }

  uint64_t data = (uint64_t)(r.p - m->file);
  data = (data + align - 1) & ~(align - 1);

  for (uint32_t i = 0; i < m->n_tensors; i++) {
    struct llm_tensor *t = &m->tensors[i];
    uint64_t elements = 1;
    int ok;

    for (uint32_t d = 0; d < t->n_dims; d++) {
      if (t->ne[d] == 0 || elements > (1ull << 40) / t->ne[d]) { ok = 0; goto bad_tensor; }
      elements *= t->ne[d];
    }

    uint64_t at = data + t->bytes;
    uint64_t bytes = tensor_bytes(t->type, elements, &ok);

    if (!ok) continue;                     /* a type this does not read: only an error if used */
    if (at < data || at > size || bytes > size - at) {
      ok = 0;
bad_tensor:
      say(why, room, "a tensor lies outside the file");
      return -1;
    }

    t->data = m->file + at;
    t->bytes = bytes;
  }

  m->arch = meta_str(m, "general.architecture");
  m->name = meta_str(m, "general.name");

  if (str_is(m->arch, "phi2")) m->kind = LLM_ARCH_PHI2;
  else if (str_is(m->arch, "qwen2")) m->kind = LLM_ARCH_QWEN2;
  else {
    say(why, room, "an architecture this does not run yet (phi2 and qwen2 are)");
    return -1;
  }

  const int qwen = m->kind == LLM_ARCH_QWEN2;

  m->n_embd = (uint32_t)arch_number(m, "embedding_length", 0);
  m->n_head = (uint32_t)arch_number(m, "attention.head_count", 0);
  m->n_head_kv = (uint32_t)arch_number(m, "attention.head_count_kv", m->n_head);
  m->n_layer = (uint32_t)arch_number(m, "block_count", 0);
  m->n_ff = (uint32_t)arch_number(m, "feed_forward_length", 0);
  m->n_ctx_train = (uint32_t)arch_number(m, "context_length", 2048);
  m->norm_eps = qwen ? (float)arch_number(m, "attention.layer_norm_rms_epsilon", 1e-6)
                     : (float)arch_number(m, "attention.layer_norm_epsilon", 1e-5);
  m->rope_base = (float)arch_number(m, "rope.freq_base", 10000.0);

  m->tok_embd = tensor(m, "token_embd.weight");
  m->n_vocab = m->tok_embd && m->tok_embd->n_dims == 2 ? (uint32_t)m->tok_embd->ne[1] : 0;

  /* Fewer key heads than query heads is grouped-query attention: each
   * key head serves `n_head / n_head_kv` query heads, side by side. */
  if (!m->n_embd || !m->n_head || m->n_embd % m->n_head || !m->n_head_kv
      || m->n_head_kv > m->n_head || m->n_head % m->n_head_kv
      || !m->n_layer || m->n_layer > 1024 || !m->n_ff || m->n_ff > (1u << 20)
      || m->n_embd > (1u << 16) || !m->n_vocab || m->n_vocab > (1u << 20)) {
    say(why, room, "its shape is missing, or not one this runs");
    return -1;
  }

  m->head_dim = m->n_embd / m->n_head;
  m->kv_dim = m->n_head_kv * m->head_dim;
  m->n_rot = (uint32_t)arch_number(m, "rope.dimension_count", m->head_dim);
  if (m->n_rot > m->head_dim || (m->n_rot & 1)) { say(why, room, "a bad rotary size"); return -1; }
  if (m->head_dim > LLM_HEAD_DIM_MAX) { say(why, room, "a head wider than the kit's attention takes"); return -1; }

  uint32_t E = m->n_embd, KV = m->kv_dim;

  m->out_norm_w = tensor(m, "output_norm.weight");
  m->out_norm_b = qwen ? NULL : tensor(m, "output_norm.bias");
  m->out_w = tensor(m, "output.weight");
  m->out_b = tensor(m, "output.bias");

  /* Qwen2's output may be the embedding itself, read the other way
   * (`TENSOR_DUPLICATED`): the 0.5B's is a matrix of its own, larger
   * models' are not. */
  if (qwen && !m->out_w) m->out_w = m->tok_embd;

  if (!is_matrix(m->tok_embd, E, m->n_vocab) || m->tok_embd->type == LLM_F32
      || !is_vector(m->out_norm_w, E) || !(qwen || is_vector(m->out_norm_b, E))
      || !is_matrix(m->out_w, E, m->n_vocab)
      || !(qwen ? !m->out_b || is_vector(m->out_b, m->n_vocab) : is_vector(m->out_b, m->n_vocab))) {
    say(why, room, "its embedding or output tensors are missing or the wrong shape");
    return -1;
  }

  m->layers = alloc(ctx, m->n_layer * sizeof *m->layers);
  if (!m->layers) { say(why, room, "no memory for its layers"); return -1; }

  for (uint32_t l = 0; l < m->n_layer; l++) {
    struct llm_layer *L = &m->layers[l];

    L->norm_w = layer_tensor(m, l, "attn_norm.weight");
    L->norm_b = layer_tensor(m, l, "attn_norm.bias");
    L->qkv_w = layer_tensor(m, l, "attn_qkv.weight");
    L->qkv_b = layer_tensor(m, l, "attn_qkv.bias");
    L->q_w = layer_tensor(m, l, "attn_q.weight");
    L->q_b = layer_tensor(m, l, "attn_q.bias");
    L->k_w = layer_tensor(m, l, "attn_k.weight");
    L->k_b = layer_tensor(m, l, "attn_k.bias");
    L->v_w = layer_tensor(m, l, "attn_v.weight");
    L->v_b = layer_tensor(m, l, "attn_v.bias");
    L->o_w = layer_tensor(m, l, "attn_output.weight");
    L->o_b = layer_tensor(m, l, "attn_output.bias");
    L->up_w = layer_tensor(m, l, "ffn_up.weight");
    L->up_b = layer_tensor(m, l, "ffn_up.bias");
    L->down_w = layer_tensor(m, l, "ffn_down.weight");
    L->down_b = layer_tensor(m, l, "ffn_down.bias");

    if (qwen) {
      /* RMSNorm, so no norm bias; Q, K and V apart, each with a bias or
       * none; a second norm before the MLP, and its gate. */
      L->norm_b = L->qkv_w = L->qkv_b = NULL;
      L->ffn_norm_w = layer_tensor(m, l, "ffn_norm.weight");
      L->gate_w = layer_tensor(m, l, "ffn_gate.weight");

      if (!is_vector(L->norm_w, E)
          || !is_matrix(L->q_w, E, E) || (L->q_b && !is_vector(L->q_b, E))
          || !is_matrix(L->k_w, E, KV) || (L->k_b && !is_vector(L->k_b, KV))
          || !is_matrix(L->v_w, E, KV) || (L->v_b && !is_vector(L->v_b, KV))
          || !is_matrix(L->o_w, E, E) || (L->o_b && !is_vector(L->o_b, E))
          || !is_vector(L->ffn_norm_w, E)
          || !is_matrix(L->gate_w, E, m->n_ff) || !is_matrix(L->up_w, E, m->n_ff)
          || !is_matrix(L->down_w, m->n_ff, E) || L->up_b || L->down_b) {
        say(why, room, "a layer's tensors are missing or the wrong shape");
        return -1;
      }
      continue;
    }

    int fused = is_matrix(L->qkv_w, E, 3 * E) && is_vector(L->qkv_b, 3 * E);
    int apart = is_matrix(L->q_w, E, E) && is_vector(L->q_b, E)
                && is_matrix(L->k_w, E, E) && is_vector(L->k_b, E)
                && is_matrix(L->v_w, E, E) && is_vector(L->v_b, E);

    if (KV != E || !is_vector(L->norm_w, E) || !is_vector(L->norm_b, E) || !(fused || apart)
        || !is_matrix(L->o_w, E, E) || !is_vector(L->o_b, E)
        || !is_matrix(L->up_w, E, m->n_ff) || !is_vector(L->up_b, m->n_ff)
        || !is_matrix(L->down_w, m->n_ff, E) || !is_vector(L->down_b, E)) {
      say(why, room, "a layer's tensors are missing or the wrong shape");
      return -1;
    }
    if (!fused) L->qkv_w = L->qkv_b = NULL;
  }

  if (open_tokenizer(m, alloc, ctx, why, room) < 0) return -1;

  /* The cache a model is given unasked is its own context, up to
   * `LLM_CTX_DEFAULT`: phi-2's 2048 whole, and Qwen2.5's 32768 - a 400 MB
   * cache - cut to what a conversation here reaches. */
  m->n_ctx = n_ctx ? n_ctx : m->n_ctx_train < LLM_CTX_DEFAULT ? m->n_ctx_train : LLM_CTX_DEFAULT;
  if (m->n_ctx > m->n_ctx_train) m->n_ctx = m->n_ctx_train;

  size_t cache = (size_t)m->n_layer * m->n_ctx * KV;
  uint32_t widest = m->n_ff > 3 * E ? m->n_ff : 3 * E;

  m->k_cache = alloc(ctx, cache * 2);
  m->v_cache = alloc(ctx, cache * 2);
  /* A batch's worth of each working vector (`llm_step_batch`). */
  const size_t B = LLM_BATCH;

  m->xq_room = widest / LLM_QK * 34 + 34;
  m->xs_room = widest / LLM_QK + 1;
  m->x = alloc(ctx, B * E * 4);
  m->xn = alloc(ctx, B * E * 4);
  m->qkv = alloc(ctx, B * 3 * E * 4);
  m->att = alloc(ctx, (size_t)m->n_ctx * 4 * LLM_ATTENTION_PARTS);
  m->attn_out = alloc(ctx, B * E * 4);
  m->ff = alloc(ctx, B * m->n_ff * 4);
  m->ff_out = alloc(ctx, B * E * 4);
  m->logits = alloc(ctx, (size_t)m->n_vocab * 4);
  m->up = qwen ? alloc(ctx, B * m->n_ff * 4) : NULL;
  m->xq = alloc(ctx, B * m->xq_room);
  m->xs = alloc(ctx, B * m->xs_room * 4);

  if (!m->k_cache || !m->v_cache || !m->x || !m->xn || !m->qkv || !m->att || !m->attn_out
      || !m->ff || !m->ff_out || !m->logits || !m->xq || !m->xs || (qwen && !m->up)) {
    say(why, room, "no memory for its attention cache and working vectors");
    return -1;
  }

  (void)llm_use_loops(m, LLM_KIT_LOOPS_AUTO);

  return 0;
}

/* ------------------------------------------------------------------ */
/* The tokenizer                                                       */
/* ------------------------------------------------------------------ */

/* GPT-2's bytes as characters: printable ones as themselves, the rest from
 * U+0100 up, in byte order (`bytes_to_unicode`). */
static uint32_t byte_char(uint8_t b)
{
  if ((b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174)) return b;

  uint32_t n = 0;
  for (uint32_t c = 0; c < b; c++)
    if (!((c >= 33 && c <= 126) || (c >= 161 && c <= 172) || (c >= 174))) n++;
  return 256 + n;
}

static int char_byte(uint32_t c)
{
  if (c < 256) {
    uint8_t b = (uint8_t)c;
    return ((b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174)) ? b : -1;
  }
  uint32_t n = 256;
  for (uint32_t b = 0; b < 256; b++) {
    if ((b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174)) continue;
    if (n++ == c) return (int)b;
  }
  return -1;
}

static int put_utf8(uint32_t c, char *out)
{
  if (c < 0x80) { out[0] = (char)c; return 1; }
  if (c < 0x800) { out[0] = (char)(0xc0 | c >> 6); out[1] = (char)(0x80 | (c & 0x3f)); return 2; }
  if (c < 0x10000) {
    out[0] = (char)(0xe0 | c >> 12); out[1] = (char)(0x80 | ((c >> 6) & 0x3f));
    out[2] = (char)(0x80 | (c & 0x3f)); return 3;
  }
  out[0] = (char)(0xf0 | c >> 18); out[1] = (char)(0x80 | ((c >> 12) & 0x3f));
  out[2] = (char)(0x80 | ((c >> 6) & 0x3f)); out[3] = (char)(0x80 | (c & 0x3f));
  return 4;
}

/* One character from `s`, as llama.cpp reads one: a malformed byte is
 * U+FFFD and one byte long. */
static uint32_t get_utf8(const char *s, size_t n, size_t *len)
{
  const uint8_t *u = (const uint8_t *)s;
  *len = 1;
  if (u[0] < 0x80) return u[0];

  size_t need = (u[0] & 0xe0) == 0xc0 ? 2 : (u[0] & 0xf0) == 0xe0 ? 3 : (u[0] & 0xf8) == 0xf0 ? 4 : 0;
  if (!need || need > n) return 0xfffd;
  for (size_t i = 1; i < need; i++) if ((u[i] & 0xc0) != 0x80) return 0xfffd;

  uint32_t c = need == 2 ? u[0] & 0x1f : need == 3 ? u[0] & 0x0f : u[0] & 0x07;
  for (size_t i = 1; i < need; i++) c = c << 6 | (u[i] & 0x3f);
  *len = need;
  return c;
}

/*
 * The classes GPT-2's split asks about. Exact for ASCII and Latin-1, which
 * is what the host test holds to llama.cpp; above U+00FF, whitespace is
 * Unicode's list, a few blocks of punctuation and digits are named, and
 * the rest is taken as letters - a model's text is mostly letters there.
 */
enum { C_NONE, C_LETTER, C_NUMBER, C_SPACE, C_OTHER };

static int char_class(uint32_t c)
{
  if (c == 0xffffffffu) return C_NONE;
  if (c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32 || c == 0x85 || c == 0xa0
      || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029
      || c == 0x202f || c == 0x205f || c == 0x3000)
    return C_SPACE;
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return C_LETTER;
  if (c >= '0' && c <= '9') return C_NUMBER;
  if (c < 0xa0) return C_OTHER;
  if (c < 0x100) {
    if (c == 0xaa || c == 0xb5 || c == 0xba) return C_LETTER;
    if (c == 0xb2 || c == 0xb3 || c == 0xb9 || (c >= 0xbc && c <= 0xbe)) return C_NUMBER;
    if (c >= 0xc0 && c != 0xd7 && c != 0xf7) return C_LETTER;
    return C_OTHER;
  }
  if ((c >= 0x2000 && c <= 0x2bff) || (c >= 0x3000 && c <= 0x303f) || (c >= 0xfe30 && c <= 0xfe4f)
      || (c >= 0xff00 && c <= 0xff0f) || (c >= 0x1f000 && c <= 0x1faff))
    return C_OTHER;
  if ((c >= 0x660 && c <= 0x669) || (c >= 0x966 && c <= 0x96f) || (c >= 0xff10 && c <= 0xff19))
    return C_NUMBER;
  return C_LETTER;
}

struct out {
  int32_t *ids;
  int room, n;
};

static void emit(struct out *o, int32_t id)
{
  if (o->n < o->room) o->ids[o->n] = id;
  o->n++;
}

#define WORD_MAX 1024              /* characters of one word BPE merges at once */

/* One word, already GPT-2's characters, through BPE: the lowest-ranked
 * pair merged first, the leftmost of equal ranks - llama.cpp's queue. */
static void bpe_word(const struct llm_model *m, const char *w, uint32_t n, struct out *o)
{
  uint32_t start[WORD_MAX], len[WORD_MAX];
  uint32_t k = 0;
  char pair[512];

  for (uint32_t at = 0; at < n && k < WORD_MAX;) {
    size_t l;
    get_utf8(w + at, n - at, &l);
    start[k] = at;
    len[k] = (uint32_t)l;
    k++;
    at += (uint32_t)l;
  }

  for (;;) {
    int32_t best = -1;
    uint32_t where = 0;

    for (uint32_t i = 0; i + 1 < k; i++) {
      uint32_t a = len[i], b = len[i + 1];
      if (a + 1 + b > sizeof pair) continue;
      memcpy(pair, w + start[i], a);
      pair[a] = ' ';
      memcpy(pair + a + 1, w + start[i + 1], b);
      int32_t r = merge_rank(m, pair, a + 1 + b);
      if (r >= 0 && (best < 0 || r < best)) { best = r; where = i; }
    }

    if (best < 0) break;

    len[where] += len[where + 1];
    for (uint32_t i = where + 1; i + 1 < k; i++) { start[i] = start[i + 1]; len[i] = len[i + 1]; }
    k--;
  }

  for (uint32_t i = 0; i < k; i++) {
    int32_t id = token_id(m, w + start[i], len[i]);

    if (id >= 0) { emit(o, id); continue; }

    /* Not a token: its bytes, each its own (`byte_to_token`). */
    for (uint32_t j = 0; j < len[i]; j++) {
      char c[2];
      int cl = put_utf8(byte_char((uint8_t)w[start[i] + j]), c);
      id = token_id(m, c, (uint32_t)cl);
      if (id >= 0) emit(o, id);
    }
  }
}

/* The bytes from `i` to `end` as one word: GPT-2's characters, each byte
 * of its UTF-8 mapped, and then BPE. */
static void bpe_span(const struct llm_model *m, const char *s, size_t i, size_t end, struct out *o)
{
  char w[WORD_MAX * 2];
  uint32_t wn = 0;
  size_t at = i;

  while (at < end) {
    size_t l;
    char u[4];
    uint32_t c = get_utf8(s + at, end - at, &l);
    int ul = put_utf8(c, u);
    for (int b = 0; b < ul; b++) {
      if (wn + 2 > sizeof w) {               /* a very long word: in pieces */
        bpe_word(m, w, wn, o);
        wn = 0;
      }
      wn += (uint32_t)put_utf8(byte_char((uint8_t)u[b]), w + wn);
    }
    at += l;
  }
  if (wn) bpe_word(m, w, wn, o);
}

/* The character at `at`, or none past `n`. */
static uint32_t char_at(const char *s, size_t n, size_t at, size_t *len)
{
  *len = 0;
  return at < n ? get_utf8(s + at, n - at, len) : 0xffffffffu;
}

static uint32_t ascii_lower(uint32_t c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/*
 * **Qwen2's split** - `(?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+
 * |\p{N}| ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+` - read as
 * llama.cpp's `unicode_regex_split_custom_qwen2` reads it, a rule at a
 * time from each position. Where GPT-2's differs: a contraction in any
 * case; a letter run takes one character before it that is not a letter,
 * a digit or a line's end - punctuation as well as a space; digits one at
 * a time; and a run of whitespace that holds a line's end ends after the
 * last of them.
 */
static void tokenize_qwen2(const struct llm_model *m, const char *s, size_t n, struct out *o)
{
  size_t pos = 0;

  while (pos < n) {
    size_t l0, l1, l2, at;
    uint32_t c0 = char_at(s, n, pos, &l0);
    uint32_t c1 = char_at(s, n, pos + l0, &l1);
    int f0 = char_class(c0);
    size_t end;

    if (c0 == '\'' && c1 != 0xffffffffu) {
      uint32_t n1 = ascii_lower(c1);
      if (n1 == 's' || n1 == 't' || n1 == 'm' || n1 == 'd') { end = pos + l0 + l1; goto word; }
      uint32_t n2 = ascii_lower(char_at(s, n, pos + l0 + l1, &l2));
      if ((n1 == 'r' && n2 == 'e') || (n1 == 'v' && n2 == 'e') || (n1 == 'l' && n2 == 'l')) {
        end = pos + l0 + l1 + l2;
        goto word;
      }
    }

    /* [^\r\n\p{L}\p{N}]?\p{L}+ */
    if (!(c0 == '\r' || c0 == '\n' || f0 == C_NUMBER)
        && (f0 == C_LETTER || char_class(c1) == C_LETTER)) {
      at = pos + l0;
      for (;;) {
        size_t l;
        uint32_t c = char_at(s, n, at, &l);
        if (char_class(c) != C_LETTER) break;
        at += l;
      }
      end = at;
      goto word;
    }

    /* \p{N} */
    if (f0 == C_NUMBER) { end = pos + l0; goto word; }

    /* ' ?[^\s\p{L}\p{N}]+[\r\n]*' */
    {
      int f2 = c0 == ' ' ? char_class(c1) : f0;

      if (f2 == C_OTHER) {
        size_t l;
        at = pos + (c0 == ' ' ? l0 : 0);
        while (char_class(char_at(s, n, at, &l)) == C_OTHER) at += l;
        for (uint32_t c = char_at(s, n, at, &l); c == '\r' || c == '\n'; c = char_at(s, n, at, &l)) at += l;
        end = at;
        goto word;
      }
    }

    /* Whitespace: \s*[\r\n]+, then \s+(?!\S), then \s+. */
    {
      size_t count = 0, last = pos, after_line = 0, l;
      uint32_t c;

      at = pos;
      while (char_class(c = char_at(s, n, at, &l)) == C_SPACE) {
        last = at;
        at += l;
        if (c == '\r' || c == '\n') after_line = at;
        count++;
      }

      if (after_line) end = after_line;
      else if (count > 1 && at < n) end = last;
      else if (count > 0) end = at;
      else end = pos + l0;                  /* no match: one character */
    }

word:
    bpe_span(m, s, pos, end, o);
    pos = end;
  }
}

/* A run of text with no special token in it: the model's split, then BPE. */
static void tokenize_plain(const struct llm_model *m, const char *s, size_t n, struct out *o)
{
  if (m->qwen2_split) {
    tokenize_qwen2(m, s, n, o);
    return;
  }

  size_t i = 0;

  while (i < n) {
    /* Where this word ends, by GPT-2's regex, read as llama.cpp's
     * `unicode_regex_split_custom_gpt2` reads it. */
    size_t l0, l1, l2;
    uint32_t c0 = get_utf8(s + i, n - i, &l0);
    uint32_t c1 = i + l0 < n ? get_utf8(s + i + l0, n - i - l0, &l1) : 0xffffffffu;
    size_t end = i;

    if (c0 == '\'' && c1 != 0xffffffffu) {
      uint32_t c2 = i + l0 + l1 < n ? get_utf8(s + i + l0 + l1, n - i - l0 - l1, &l2) : 0xffffffffu;
      if (c1 == 's' || c1 == 't' || c1 == 'm' || c1 == 'd') { end = i + l0 + l1; goto word; }
      if ((c1 == 'r' && c2 == 'e') || (c1 == 'v' && c2 == 'e') || (c1 == 'l' && c2 == 'l')) {
        end = i + l0 + l1 + l2;
        goto word;
      }
    }

    {
      int cls = char_class(c0 == ' ' ? c1 : c0);
      size_t at = i + (c0 == ' ' ? l0 : 0);

      if (cls == C_LETTER || cls == C_NUMBER || cls == C_OTHER) {
        if (c0 != ' ') at = i;
        while (at < n) {
          size_t l;
          uint32_t c = get_utf8(s + at, n - at, &l);
          if (char_class(c) != cls) break;
          at += l;
        }
        end = at;
        goto word;
      }
    }

    {
      /* Whitespace: a run, less its last character when something follows. */
      size_t at = i, last = i, count = 0;
      while (at < n) {
        size_t l;
        uint32_t c = get_utf8(s + at, n - at, &l);
        if (char_class(c) != C_SPACE) break;
        last = at;
        at += l;
        count++;
      }
      if (count > 1 && at < n) end = last;
      else if (count > 0) end = at;
      else end = i + l0;                    /* no match: one character */
    }

word:
    bpe_span(m, s, i, end, o);
    i = end;
  }
}

#define FRAGMENTS 512

int llm_tokenize(const struct llm_model *m, const char *text, size_t n, int special,
                 int32_t *ids, int room)
{
  /* llama.cpp's `tokenizer_st_partition`: every special token in turn,
   * longest first, cut out of whatever plain text is left. */
  struct { size_t at, n; int32_t id; } frag[FRAGMENTS];
  int nf = 1;
  struct out o = { ids, room, 0 };

  frag[0].at = 0;
  frag[0].n = n;
  frag[0].id = -1;

  for (uint32_t s = 0; s < m->n_specials; s++) {
    uint32_t id = m->specials[s];
    struct llm_str t = m->tokens[id];

    if (m->token_type[id] == 3 && !special) continue;

    for (int f = 0; f < nf; f++) {
      if (frag[f].id >= 0 || frag[f].n < t.n) continue;

      size_t base = frag[f].at, left = frag[f].n;

      for (size_t p = 0; p + t.n <= left; p++) {
        if (memcmp(text + base + p, t.s, t.n) != 0) continue;
        if (nf + 2 > FRAGMENTS) return -1;

        /* [before][token][after]: `f` keeps before, if any. */
        memmove(&frag[f + 3], &frag[f + 1], (size_t)(nf - f - 1) * sizeof frag[0]);
        frag[f + 1].at = base + p; frag[f + 1].n = t.n; frag[f + 1].id = (int32_t)id;
        frag[f + 2].at = base + p + t.n; frag[f + 2].n = left - p - t.n; frag[f + 2].id = -1;
        frag[f].n = p;
        nf += 2;

        if (p == 0) {                         /* nothing before: drop the empty one */
          memmove(&frag[f], &frag[f + 1], (size_t)(nf - f - 1) * sizeof frag[0]);
          nf--;
        } else {
          f++;
        }
        f++;                                  /* now the text after */
        if (frag[f].n == 0) {
          memmove(&frag[f], &frag[f + 1], (size_t)(nf - f - 1) * sizeof frag[0]);
          nf--;
          break;
        }
        base = frag[f].at;
        left = frag[f].n;
        p = (size_t)-1;
      }
    }
  }

  for (int f = 0; f < nf; f++) {
    if (frag[f].id >= 0) emit(&o, frag[f].id);
    else if (frag[f].n) tokenize_plain(m, text + frag[f].at, frag[f].n, &o);
  }

  return o.n <= room ? o.n : -1;
}

int llm_token_text(const struct llm_model *m, int32_t token, char *out, int room)
{
  if (token < 0 || (uint32_t)token >= m->n_vocab) return 0;

  struct llm_str t = m->tokens[token];
  int n = 0;

  /* A control token says nothing; a user-defined one is its text. */
  if (m->token_type[token] == 3) return 0;
  if (m->token_type[token] == 4) {
    for (uint32_t i = 0; i < t.n && n < room; i++) out[n++] = t.s[i];
    return n;
  }

  for (size_t i = 0; i < t.n;) {
    size_t l;
    uint32_t c = get_utf8(t.s + i, t.n - i, &l);
    int b = char_byte(c);
    if (b >= 0 && n < room) out[n++] = (char)b;
    i += l;
  }
  return n;
}

/* ------------------------------------------------------------------ */
/* Arithmetic                                                          */
/* ------------------------------------------------------------------ */

/* Round to nearest, ties to even: NEON's `vcvtnq`, which ggml's Q8_0
 * quantiser uses on the Mac, and SSE's default rounding on a PC. */
static int32_t round_even(float v)
{
  float r = floorf(v + 0.5f);
  if (r - v == 0.5f && ((int32_t)r & 1)) r -= 1.0f;
  return (int32_t)r;
}

/* `n` floats as Q8_0 blocks (`quantize_row_q8_0`), and each block's scale
 * as the float it stands for into `xs`, once for every row it meets. */
static void quantize_q8_0(const float *x, uint8_t *out, float *xs, uint32_t n)
{
  for (uint32_t b = 0; b < n / LLM_QK; b++) {
    const float *v = x + b * LLM_QK;
    float amax = 0;
    for (int i = 0; i < LLM_QK; i++) { float a = fabsf(v[i]); if (a > amax) amax = a; }

    float d = amax / 127.0f;
    float id = d != 0.0f ? 1.0f / d : 0.0f;
    uint16_t dh = llm_f32_to_f16(d);

    xs[b] = llm_f16_to_f32(dh);
    out[b * 34] = (uint8_t)dh;
    out[b * 34 + 1] = (uint8_t)(dh >> 8);
    for (int i = 0; i < LLM_QK; i++) out[b * 34 + 2 + i] = (uint8_t)(int8_t)round_even(v[i] * id);
  }
}

static float block_scale(const uint8_t *b) { return llm_f16_to_f32((uint16_t)(b[0] | b[1] << 8)); }

/* One row of a weight times the input, as ggml multiplies it: `x` the
 * input as floats, `xq` and `xs` as Q8_0 blocks and their scales. */
static float row_dot(const struct llm_model *m, const struct llm_tensor *w, uint64_t row,
                     const float *x, const uint8_t *xq, const float *xs)
{
  uint64_t cols = w->ne[0];

  switch (w->type) {
    case LLM_Q8_0:
      return m->dot_q8_0(w->data + row * (cols / LLM_QK) * 34, xq, xs, cols / LLM_QK);
    case LLM_Q4_0:
      return m->dot_q4_0(w->data + row * (cols / LLM_QK) * 18, xq, xs, cols / LLM_QK);
    case LLM_F16: {
      const uint8_t *r = w->data + row * cols * 2;
      double sum = 0;
      for (uint64_t i = 0; i < cols; i++)
        sum += (double)(llm_f16_to_f32((uint16_t)(r[2 * i] | r[2 * i + 1] << 8))
                        * llm_f16_to_f32(llm_f32_to_f16(x[i])));
      return (float)sum;
    }
    default: {
      const float *r = (const float *)(const void *)(w->data + row * cols * 4);
      double sum = 0;
      for (uint64_t i = 0; i < cols; i++) sum += (double)(r[i] * x[i]);
      return (float)sum;
    }
  }
}

/* A product, as `matmul` hands it out: everything a part needs. */
struct product {
  struct llm_model *m;
  const struct llm_tensor *w;
  const float *b, *x;
  size_t x_room, out_room;
  float *out;
  uint32_t n;
  uint64_t first;                /* the rows before it are another's (X7 G5) */
};

/* Rows `r0` up to `r1` of a product, for every input. */
static void product_rows(const struct product *p, uint64_t r0, uint64_t r1)
{
  struct llm_model *m = p->m;
  const struct llm_tensor *w = p->w;
  const float *b = p->b;
  llm_dot4_fn *four = w->type == LLM_Q8_0 ? m->dot4_q8_0 : w->type == LLM_Q4_0 ? m->dot4_q4_0 : NULL;
  const uint64_t row_bytes = w->type == LLM_Q8_0 ? w->ne[0] / LLM_QK * 34 : w->ne[0] / LLM_QK * 18;

  for (uint64_t r = r0; r < r1; r++) {
    uint32_t t = 0;

    /* Four inputs at a time where the row's kind has the loop for it. */
    for (; four && t + LLM_DOT4 <= p->n; t += LLM_DOT4) {
      float v[LLM_DOT4];

      four(w->data + r * row_bytes, m->xq + t * m->xq_room, m->xq_room, m->xs + t * m->xs_room,
           m->xs_room, w->ne[0] / LLM_QK, v);
      for (int i = 0; i < LLM_DOT4; i++) p->out[(t + i) * p->out_room + r] = b ? v[i] + b[r] : v[i];
    }

    for (; t < p->n; t++) {
      float v = row_dot(m, w, r, p->x + t * p->x_room, m->xq + t * m->xq_room,
                        m->xs + t * m->xs_room);
      p->out[t * p->out_room + r] = b ? v + b[r] : v;
    }
  }
}

/* Part `part` of `parts`: its share of the rows, cut evenly. */
static void product_part(void *job, uint32_t part, uint32_t parts)
{
  const struct product *p = job;
  uint64_t rows = p->w->ne[1] - p->first;

  product_rows(p, p->first + rows * part / parts, p->first + rows * (part + 1) / parts);
}

/*
 * out = W x + b for each of `n` inputs: input `t` at `x + t * x_room`, its
 * answer at `out + t * out_room`. With `quantised`, `m->xq` already holds
 * these inputs as Q8_0, from the product before over the same ones.
 *
 * **The row outside, the inputs inside** - that is the whole of batching.
 * A row is read from memory once and met by every input while it is in the
 * cache, where one input at a time read every row again for each. Each
 * (row, input) is the same `row_dot` on the same bytes either way, so the
 * order the loops run in changes no answer.
 *
 * **And the rows cut across the cores**, where the caller gave a way to
 * (`llm_parallel`): each part a run of whole rows, so no sum is split and
 * none is taken in another order. A product of fewer than 256 rows a part
 * stays on this thread, where handing it out would cost more than it is.
 */
static void matmul(struct llm_model *m, const struct llm_tensor *w, const struct llm_tensor *bias,
                   const float *x, size_t x_room, float *out, size_t out_room, uint32_t n,
                   int quantised)
{
  if (!quantised && (w->type == LLM_Q8_0 || w->type == LLM_Q4_0))
    for (uint32_t t = 0; t < n; t++)
      quantize_q8_0(x + t * x_room, m->xq + t * m->xq_room, m->xs + t * m->xs_room,
                    (uint32_t)w->ne[0]);

  struct product p = { m, w, bias ? (const float *)(const void *)bias->data : NULL, x, x_room,
                       out_room, out, n, 0 };
  int64_t begun = -1;

  /* Offered to the GPU first (X7): the first rows begun there, the same
   * sums to the bit, and the rest here at the same time (G5). */
  if (m->product && w->type == LLM_Q4_0)
    begun = m->product(m->product_ctx, w, m->xq, m->xq_room, m->xs, m->xs_room, n, p.b, out, out_room);

  if (begun > 0) p.first = (uint64_t)begun < w->ne[1] ? (uint64_t)begun : w->ne[1];

  if (p.first < w->ne[1]) {
    if (m->run && m->parts > 1 && w->ne[1] - p.first >= 256u * m->parts)
      m->run(m->run_ctx, product_part, &p, m->parts);
    else
      product_rows(&p, p.first, w->ne[1]);
  }

  if (begun > 0 && m->product_done(m->product_ctx) != 0) product_rows(&p, 0, p.first);
}

void llm_offload(struct llm_model *m, llm_product_fn *product, llm_product_done_fn *done, void *ctx)
{
  m->product = product;
  m->product_done = done;
  m->product_ctx = ctx;
}

unsigned llm_loops_have(void)
{
  unsigned have = 1u << LLM_KIT_LOOPS_PLAIN | 1u << LLM_KIT_LOOPS_BASE;

#if defined(__x86_64__)
  /* AVX2 where the processor has it and the system keeps it: the same
   * answers to the bit (`llm_dot_avx2.c`), a block in one register. */
  if (llm_avx2_present()) have |= 1u << LLM_KIT_LOOPS_AVX2;
#endif

  return have;
}

int llm_use_loops(struct llm_model *m, unsigned which)
{
  const unsigned have = llm_loops_have();

  if (which == LLM_KIT_LOOPS_AUTO) {
    which = have & 1u << LLM_KIT_LOOPS_AVX2 ? LLM_KIT_LOOPS_AVX2 : LLM_KIT_LOOPS_BASE;
  }

  if (which > LLM_KIT_LOOPS_AVX2 || !(have & 1u << which)) return -1;

  switch (which) {
  case LLM_KIT_LOOPS_PLAIN:
    /* The reference: one input at a time, scalar, every row. */
    m->dot_q8_0 = llm_dot_q8_0_scalar;
    m->dot_q4_0 = llm_dot_q4_0_scalar;
    m->dot4_q8_0 = m->dot4_q4_0 = NULL;
    break;
#if defined(__x86_64__)
  case LLM_KIT_LOOPS_AVX2:
    m->dot_q8_0 = llm_dot_q8_0_avx2;
    m->dot_q4_0 = llm_dot_q4_0_avx2;
    m->dot4_q8_0 = llm_dot4_q8_0_avx2;
    m->dot4_q4_0 = llm_dot4_q4_0_avx2;
    break;
#endif
  default:
    /* What the kit chose before there was a choice: the plain loop for one
     * input, the fastest wherever it has been measured, and the four-input
     * loops, which the compiler makes the core's vectors. */
    m->dot_q8_0 = llm_dot_q8_0_scalar;
    m->dot_q4_0 = llm_dot_q4_0_scalar;
    m->dot4_q8_0 = llm_dot4_q8_0;
    m->dot4_q4_0 = llm_dot4_q4_0;
    break;
  }

  m->loops = which;
  return (int)which;
}

void llm_clock(struct llm_model *m, llm_clock_fn *clock)
{
  m->clock = clock;
}

/* The time since `*since` to `stage`, and the clock read again: called as
 * each stage ends, so the stages tile the pass with nothing between. */
static inline void lap(struct llm_model *m, uint64_t *since, uint32_t stage)
{
  if (m->clock) {
    uint64_t now = m->clock();

    m->stage_counts[stage] += now - *since;
    *since = now;
  }
}

void llm_parallel(struct llm_model *m, llm_run_fn *run, void *ctx, uint32_t parts)
{
  m->run = parts > 1 ? run : NULL;
  m->run_ctx = ctx;
  m->parts = parts;
}

/* A row of the embedding, as floats (`get_rows`, `dequantize_row_*`). */
static void embedding_row(const struct llm_tensor *t, uint32_t row, float *out)
{
  uint64_t cols = t->ne[0];

  if (t->type == LLM_Q8_0) {
    const uint8_t *r = t->data + (uint64_t)row * (cols / LLM_QK) * 34;
    for (uint64_t b = 0; b < cols / LLM_QK; b++) {
      float d = block_scale(r + b * 34);
      for (int i = 0; i < LLM_QK; i++) out[b * LLM_QK + i] = (float)(int8_t)r[b * 34 + 2 + i] * d;
    }
  } else if (t->type == LLM_Q4_0) {
    const uint8_t *r = t->data + (uint64_t)row * (cols / LLM_QK) * 18;
    for (uint64_t b = 0; b < cols / LLM_QK; b++) {
      float d = block_scale(r + b * 18);
      for (int i = 0; i < LLM_QK / 2; i++) {
        out[b * LLM_QK + i] = (float)((r[b * 18 + 2 + i] & 0x0f) - 8) * d;
        out[b * LLM_QK + i + LLM_QK / 2] = (float)((r[b * 18 + 2 + i] >> 4) - 8) * d;
      }
    }
  } else {
    const uint8_t *r = t->data + (uint64_t)row * cols * 2;
    for (uint64_t i = 0; i < cols; i++) out[i] = llm_f16_to_f32((uint16_t)(r[2 * i] | r[2 * i + 1] << 8));
  }
}

/* LayerNorm, then the weight and the bias (`ggml_compute_forward_norm_f32`). */
static void layer_norm(const float *x, float *out, uint32_t n, const struct llm_tensor *w,
                       const struct llm_tensor *b, float eps)
{
  double sum = 0;
  for (uint32_t i = 0; i < n; i++) sum += (double)x[i];
  float mean = (float)(sum / n);

  double sum2 = 0;
  for (uint32_t i = 0; i < n; i++) { float v = x[i] - mean; out[i] = v; sum2 += (double)(v * v); }

  float variance = (float)(sum2 / n);
  float scale = 1.0f / sqrtf(variance + eps);
  const float *wv = (const float *)(const void *)w->data, *bv = (const float *)(const void *)b->data;

  for (uint32_t i = 0; i < n; i++) out[i] = out[i] * scale * wv[i] + bv[i];
}

/* RMSNorm, then the weight (`ggml_compute_forward_rms_norm_f32`, then
 * `ggml_mul`): Qwen2's norm, with no mean taken away and no bias. */
static void rms_norm(const float *x, float *out, uint32_t n, const struct llm_tensor *w, float eps)
{
  double sum = 0;
  for (uint32_t i = 0; i < n; i++) sum += (double)(x[i] * x[i]);

  float mean = (float)(sum / n);
  float scale = 1.0f / sqrtf(mean + eps);
  const float *wv = (const float *)(const void *)w->data;

  for (uint32_t i = 0; i < n; i++) out[i] = (x[i] * scale) * wv[i];
}

/* The norm a model takes: its own kind, with a bias where it has one. */
static void norm(const struct llm_model *m, const float *x, float *out, const struct llm_tensor *w,
                 const struct llm_tensor *b)
{
  if (m->kind == LLM_ARCH_QWEN2) rms_norm(x, out, m->n_embd, w, m->norm_eps);
  else layer_norm(x, out, m->n_embd, w, b, m->norm_eps);
}

/* SiLU: SwiGLU's gate is silu(gate) * up. ggml's `ggml_silu_f32` takes
 * it in float with `expf`, which Kosmos's libc has not got; in double and
 * rounded once it is the nearer answer, a float's last bit from ggml's at
 * most, and the same on the Mac and on Kosmos - as the softmax's
 * exponent is taken here. */
static float silu(float x)
{
  return (float)((double)x / (1.0 + exp(-(double)x)));
}

/* GELU through half precision, as ggml's table: the input to F16, GELU of
 * that, the answer to F16. */
static float gelu_of_half(float h)
{
  float g = 0.5f * h * (1.0f + (float)tanh((double)(0.79788456080286535587989211986876f * h
                                                      * (1.0f + 0.044715f * h * h))));
  return llm_f16_to_f32(llm_f32_to_f16(g));
}

/* **And as ggml's table it is kept**: an input is made F16 before anything
 * else, so there are 65536 inputs and as many answers, worked out once
 * (`gelu_table`, at `llm_open`). It had been a `tanh` in double for every
 * value - 327680 a token of phi-2's - and on the M700, with musl's `tanh`,
 * that was most of what the processor did while the GPU took the products
 * (`testing.md` 18.729). The answers are the same ones. */
static uint16_t gelu_f16[65536];
static int gelu_ready;

static void gelu_table(void)
{
  if (gelu_ready) return;

  for (uint32_t i = 0; i < 65536u; i++) gelu_f16[i] = llm_f32_to_f16(gelu_of_half(llm_f16_to_f32((uint16_t)i)));

  gelu_ready = 1;
}

static float gelu(float x)
{
  if (x <= -10.0f) return 0.0f;
  if (x >= 10.0f) return x;

  return llm_f16_to_f32(gelu_f16[llm_f32_to_f16(x)]);
}

float llm_gelu(float x)
{
  gelu_table();
  return gelu(x);
}

/* RoPE, NeoX's pairing - element i with i + n_rot/2 - on the first n_rot
 * of a head; the angle a running product in float (`ggml_rope_cache_init`). */
static void rope(float *head, uint32_t n_rot, uint32_t pos, float base)
{
  float scale = (float)pow((double)base, -2.0 / (double)n_rot);
  float theta = (float)pos;
  uint32_t half = n_rot / 2;

  for (uint32_t i = 0; i < half; i++) {
    float c = (float)cos((double)theta), s = (float)sin((double)theta);
    float x0 = head[i], x1 = head[i + half];
    head[i] = x0 * c - x1 * s;
    head[i + half] = x0 * s + x1 * c;
    theta *= scale;
  }
}

/* ------------------------------------------------------------------ */
/* The forward pass                                                    */
/* ------------------------------------------------------------------ */

void llm_reset(struct llm_model *m) { m->n_past = 0; }

/* ------------------------------------------------------------------------
 * What the cache holds (`llm_core.h`, `struct llm_held`).
 * --------------------------------------------------------------------- */

void llm_held_reset(struct llm_model *m, struct llm_held *h)
{
  m->n_past = 0;
  h->n = 0;
  h->text_bytes = 0;
}

/* The held text from the held tokens, as far as it fits. */
static void held_text_again(const struct llm_model *m, struct llm_held *h)
{
  uint32_t at = 0;

  for (uint32_t i = 0; i < h->n; i++) {
    char piece[256];
    int b = llm_token_text(m, h->tokens[i], piece, (int)sizeof piece);

    if (b < 0 || (uint32_t)b > h->text_room - at) {
      h->text_bytes = LLM_TEXT_UNKNOWN;
      return;
    }

    memcpy(h->text + at, piece, (size_t)b);
    at += (uint32_t)b;
  }

  h->text_bytes = at;
}

int llm_prompt(struct llm_model *m, struct llm_held *h, const char *text, size_t n,
               int32_t *out, int room, uint32_t *kept)
{
  int k;
  uint32_t same = 0;

  *kept = 0;

  if (h->n != m->n_past || h->n > m->n_ctx) llm_held_reset(m, h);

  /* The follow-up: what is held is where `text` begins, and more follows. */
  if (h->n > 0 && h->text_bytes != LLM_TEXT_UNKNOWN && n > h->text_bytes
      && memcmp(text, h->text, h->text_bytes) == 0) {
    k = llm_tokenize(m, text + h->text_bytes, n - h->text_bytes, 1, out, room);

    if (k > 0) {
      if ((uint32_t)k + h->n >= m->n_ctx) return -1;

      *kept = h->n;
      return k;
    }
  }

  /* Otherwise the whole of it, and as much of the cache as agrees. */
  k = llm_tokenize(m, text, n, 1, out, room);

  if (k <= 0 || (uint32_t)k >= m->n_ctx) return -1;

  while (same < h->n && same < (uint32_t)k && out[same] == h->tokens[same]) same++;

  if (same == (uint32_t)k) same--;      /* the last one read again, for its logits */

  m->n_past = h->n = same;
  held_text_again(m, h);
  memmove(out, out + same, ((size_t)k - same) * sizeof *out);

  *kept = same;
  return k - (int)same;
}

int llm_feed(struct llm_model *m, struct llm_held *h, int32_t token)
{
  return llm_feed_batch(m, h, &token, 1);
}

int llm_feed_batch(struct llm_model *m, struct llm_held *h, const int32_t *tokens, uint32_t n)
{
  char piece[256];
  int b;

  if (h->n != m->n_past) llm_held_reset(m, h);

  if (llm_step_batch(m, tokens, n) != 0) return -1;

  for (uint32_t t = 0; t < n; t++) {
    h->tokens[h->n++] = tokens[t];

    if (h->text_bytes == LLM_TEXT_UNKNOWN) continue;

    b = llm_token_text(m, tokens[t], piece, (int)sizeof piece);

    if (b < 0 || (uint32_t)b > h->text_room - h->text_bytes) {
      h->text_bytes = LLM_TEXT_UNKNOWN;
    } else {
      memcpy(h->text + h->text_bytes, piece, (size_t)b);
      h->text_bytes += (uint32_t)b;
    }
  }

  return 0;
}

int llm_step(struct llm_model *m, int32_t token)
{
  return llm_step_batch(m, &token, 1);
}

/*
 * **Attention for one token and one head**, over every position up to the
 * token's own: the query meets the F16 cache as F16, and so do the
 * probabilities, as ggml's `flash_attn`-less path has it.
 *
 * Each sum is taken in the same order it always was - over the head's
 * values for a score, over the positions for an output - so cutting the
 * jobs across cores, rounding the query once rather than once a position,
 * and walking the value cache a row at a time change no bit of it. Only
 * where the work happens moved (testing.md 18.733).
 */
static void attend(struct llm_model *m, uint32_t l, uint32_t t, uint32_t h, float *att,
                   const uint16_t *kc, const uint16_t *vc)
{
  const uint32_t E = m->n_embd, D = m->head_dim, KV = m->kv_dim;
  const uint32_t pos = m->n_past + t;
  const float *qh = m->qkv + (size_t)t * 3 * E + h * D;
  /* The key and value head this query head reads: its own, or in
   * grouped-query attention the one its group shares. */
  const uint32_t kvh = h / (m->n_head / m->n_head_kv);
  /* phi-2 scales the query before (`ggml_scale`); Qwen2 scales the score
   * in the softmax (`kq_scale`), and 1 times anything is itself. */
  const float score_scale = m->kind == LLM_ARCH_QWEN2 ? 1.0f / sqrtf((float)D) : 1.0f;
  float qr[LLM_HEAD_DIM_MAX];
  double acc[LLM_HEAD_DIM_MAX];
  float max = -INFINITY;

  for (uint32_t i = 0; i < D; i++) qr[i] = llm_f16_to_f32(llm_f32_to_f16(qh[i]));

  for (uint32_t p = 0; p <= pos; p++) {
    const uint16_t *kt = kc + (size_t)p * KV + kvh * D;
    double sum = 0;

    for (uint32_t i = 0; i < D; i++) sum += (double)(qr[i] * llm_f16_to_f32(kt[i]));
    att[p] = (float)sum * score_scale;
    if (att[p] > max) max = att[p];
  }

  double sum = 0;
  for (uint32_t p = 0; p <= pos; p++) {
    att[p] = (float)exp((double)(att[p] - max));
    sum += (double)att[p];
  }
  float inv = (float)(1.0 / sum);

  for (uint32_t p = 0; p <= pos; p++) att[p] = llm_f16_to_f32(llm_f32_to_f16(att[p] * inv));

  if (m->passage && l < LLM_PASSAGE_LAYERS && h < LLM_PASSAGE_HEADS) {
    uint32_t best = 0;

    for (uint32_t p = 1; p <= pos; p++)
      if (att[p] > att[best]) best = p;

    m->passage[t].layer[l].back[h] = (uint16_t)(pos - best > 65535u ? 65535u : pos - best);
    m->passage[t].layer[l].share[h] = (uint16_t)(att[best] * 65535.0f + 0.5f);
  }

  /* Each output the sum over the positions in order, as before; the cache
   * read a row at a time rather than a row apart at every step. */
  for (uint32_t i = 0; i < D; i++) acc[i] = 0;
  for (uint32_t p = 0; p <= pos; p++) {
    const uint16_t *vt = vc + (size_t)p * KV + kvh * D;
    const float a = att[p];

    for (uint32_t i = 0; i < D; i++) acc[i] += (double)(a * llm_f16_to_f32(vt[i]));
  }

  float *o = m->attn_out + (size_t)t * E + h * D;
  for (uint32_t i = 0; i < D; i++) o[i] = (float)acc[i];
}

struct attention {
  struct llm_model *m;
  uint32_t l, n;
  const uint16_t *kc, *vc;
};

/* Part `part` of a layer's attention: every `parts`-th (token, head), so
 * the late tokens, which have the most positions to look over, are shared
 * out rather than left to the last part. Each part has a row of `att` of
 * its own, and each job writes only its own head's output. */
static void attention_part(void *job, uint32_t part, uint32_t parts)
{
  const struct attention *a = job;
  struct llm_model *m = a->m;
  float *att = m->att + (size_t)part * m->n_ctx;

  for (uint32_t j = part; j < a->n * m->n_head; j += parts)
    attend(m, a->l, j / m->n_head, j % m->n_head, att, a->kc, a->vc);
}

int llm_step_batch(struct llm_model *m, const int32_t *tokens, uint32_t n)
{
  if (n == 0 || n > LLM_BATCH || m->n_past + n > m->n_ctx) return -1;
  for (uint32_t t = 0; t < n; t++)
    if (tokens[t] < 0 || (uint32_t)tokens[t] >= m->n_vocab) return -1;

  const uint32_t E = m->n_embd, D = m->head_dim, F = m->n_ff, KV = m->kv_dim;
  const int qwen = m->kind == LLM_ARCH_QWEN2;

  /* Token `t`'s working vectors. */
#define X(t)    (m->x + (size_t)(t) * E)
#define XN(t)   (m->xn + (size_t)(t) * E)
#define QKV(t)  (m->qkv + (size_t)(t) * 3 * E)
#define AOUT(t) (m->attn_out + (size_t)(t) * E)
#define FF(t)   (m->ff + (size_t)(t) * F)
#define ATTN(t) (m->ff_out + (size_t)(t) * E)
#define UP(t)   (m->up + (size_t)(t) * F)

  uint64_t since = m->clock ? m->clock() : 0;

  for (uint32_t t = 0; t < n; t++) embedding_row(m->tok_embd, (uint32_t)tokens[t], X(t));

  if (m->passage) {
    for (uint32_t t = 0; t < n; t++) {
      struct llm_passage *p = &m->passage[t];

      memset(p, 0, sizeof *p);
      p->pos = m->n_past + t;
      p->token = tokens[t];
      p->n_layer = m->n_layer;
      p->n_head = m->n_head;
      p->n_ff = F;
    }
  }
  lap(m, &since, LLM_STAGE_EMBED);

  for (uint32_t l = 0; l < m->n_layer; l++) {
    const struct llm_layer *L = &m->layers[l];

    for (uint32_t t = 0; t < n; t++) {
      norm(m, X(t), XN(t), L->norm_w, L->norm_b);
      TRACE("attn_norm", l, XN(t), E);
    }
    lap(m, &since, LLM_STAGE_NORM);

    /* Q, K and V: one product, or three over the same quantised input. */
    if (L->qkv_w) {
      matmul(m, L->qkv_w, L->qkv_b, m->xn, E, m->qkv, 3 * E, n, 0);
    } else {
      matmul(m, L->q_w, L->q_b, m->xn, E, m->qkv, 3 * E, n, 0);
      matmul(m, L->k_w, L->k_b, m->xn, E, m->qkv + E, 3 * E, n, L->k_w->type == L->q_w->type);
      matmul(m, L->v_w, L->v_b, m->xn, E, m->qkv + 2 * E, 3 * E, n, L->v_w->type == L->q_w->type);
    }
    lap(m, &since, LLM_STAGE_PRODUCTS);

    uint16_t *kc = m->k_cache + ((size_t)l * m->n_ctx) * KV;
    uint16_t *vc = m->v_cache + ((size_t)l * m->n_ctx) * KV;
    float qscale = qwen ? 1.0f : 1.0f / sqrtf((float)D);

    /* Every token's K and V into the cache first: token `t` attends to the
     * positions up to its own, and those are all written before any token
     * reads. A later one's are written too, and never read by it. */
    for (uint32_t t = 0; t < n; t++) {
      const uint32_t pos = m->n_past + t;
      float *q = QKV(t), *k = q + E, *v = q + 2 * E;

      TRACE("Qcur", l, q, E);
      TRACE("Kcur", l, k, KV);
      TRACE("Vcur", l, v, KV);

      for (uint32_t h = 0; h < m->n_head; h++) {
        rope(q + h * D, m->n_rot, pos, m->rope_base);
        if (qscale != 1.0f)
          for (uint32_t i = 0; i < D; i++) q[h * D + i] *= qscale;
      }
      for (uint32_t h = 0; h < m->n_head_kv; h++) rope(k + h * D, m->n_rot, pos, m->rope_base);

      for (uint32_t i = 0; i < KV; i++) {
        kc[(size_t)pos * KV + i] = llm_f32_to_f16(k[i]);
        vc[(size_t)pos * KV + i] = llm_f32_to_f16(v[i]);
      }
    }
    lap(m, &since, LLM_STAGE_ROPE);

    /* Attention: a (token, head) a job, the jobs dealt across the cores
     * where the caller gave a way to (`attend`). */
    {
      struct attention job = { m, l, n, kc, vc };
      uint32_t parts = m->run && m->parts > 1 && m->parts <= LLM_ATTENTION_PARTS ? m->parts : 1;

      if (parts > 1) m->run(m->run_ctx, attention_part, &job, parts);
      else attention_part(&job, 0, 1);
    }
#ifdef LLM_TRACE
    for (uint32_t t = 0; t < n; t++) TRACE("kqv_out", l, AOUT(t), E);
#endif
    lap(m, &since, LLM_STAGE_ATTENTION);

    if (qwen) {
      /* **Qwen2's layer is the two one after the other**: the attention's
       * output added to the stream, and the MLP reads a norm of that -
       * SwiGLU, silu(gate) * up, then down - and is added in its turn. */
      matmul(m, L->o_w, L->o_b, m->attn_out, E, m->ff_out, E, n, 0);
      lap(m, &since, LLM_STAGE_PRODUCTS);

      for (uint32_t t = 0; t < n; t++) {
        float *attn = ATTN(t), *x = X(t);

        TRACE("attn_out", l, attn, E);
        for (uint32_t i = 0; i < E; i++) x[i] = attn[i] + x[i];
        TRACE("ffn_inp", l, x, E);
      }
      lap(m, &since, LLM_STAGE_ADD);

      for (uint32_t t = 0; t < n; t++) {
        norm(m, X(t), XN(t), L->ffn_norm_w, NULL);
        TRACE("ffn_norm", l, XN(t), E);
      }
      lap(m, &since, LLM_STAGE_NORM);

      matmul(m, L->gate_w, NULL, m->xn, E, m->ff, F, n, 0);
      matmul(m, L->up_w, NULL, m->xn, E, m->up, F, n, L->up_w->type == L->gate_w->type);
      lap(m, &since, LLM_STAGE_PRODUCTS);

      for (uint32_t t = 0; t < n; t++) {
        float *g = FF(t), *u = UP(t);

        TRACE("ffn_gate", l, g, F);
        if (m->passage && l < LLM_PASSAGE_LAYERS) {
          uint32_t fired = 0;

          for (uint32_t i = 0; i < F; i++) fired += g[i] > 0.0f;
          m->passage[t].layer[l].fired = fired;
        }

        for (uint32_t i = 0; i < F; i++) g[i] = silu(g[i]) * u[i];
        TRACE("ffn_swiglu", l, g, F);
      }
      lap(m, &since, LLM_STAGE_GELU);

      matmul(m, L->down_w, NULL, m->ff, F, m->qkv, 3 * E, n, 0);
      lap(m, &since, LLM_STAGE_PRODUCTS);

      for (uint32_t t = 0; t < n; t++) {
        float *mlp = QKV(t), *x = X(t);

        TRACE("ffn_out", l, mlp, E);
        for (uint32_t i = 0; i < E; i++) x[i] = mlp[i] + x[i];
        TRACE("l_out", l, x, E);
      }
    } else {
      /* The attention's output, and the MLP beside it off the same norm,
       * which `xn` still holds: phi-2's layer is the two in parallel. Q,
       * K and V are done with, so the MLP's output borrows their room. */
      matmul(m, L->o_w, L->o_b, m->attn_out, E, m->ff_out, E, n, 0);
      matmul(m, L->up_w, L->up_b, m->xn, E, m->ff, F, n, 0);
      lap(m, &since, LLM_STAGE_PRODUCTS);

      for (uint32_t t = 0; t < n; t++) {
        TRACE("attn_out", l, ATTN(t), E);
        TRACE("ffn_up_b", l, FF(t), F);
        if (m->passage && l < LLM_PASSAGE_LAYERS) {
          uint32_t fired = 0;

          for (uint32_t i = 0; i < F; i++) fired += FF(t)[i] > 0.0f;
          m->passage[t].layer[l].fired = fired;
        }

        for (uint32_t i = 0; i < F; i++) FF(t)[i] = gelu(FF(t)[i]);
        TRACE("ffn_gelu", l, FF(t), F);
      }
      lap(m, &since, LLM_STAGE_GELU);

      matmul(m, L->down_w, L->down_b, m->ff, F, m->qkv, 3 * E, n, 0);
      lap(m, &since, LLM_STAGE_PRODUCTS);

      /* ggml's order: the two added, then the residual. */
      for (uint32_t t = 0; t < n; t++) {
        float *attn = ATTN(t), *mlp = QKV(t), *x = X(t);

        TRACE("ffn_out", l, mlp, E);
        for (uint32_t i = 0; i < E; i++) x[i] = (attn[i] + mlp[i]) + x[i];
        TRACE("l_out", l, x, E);
      }
    }

    if (m->passage && l < LLM_PASSAGE_LAYERS) {
      for (uint32_t t = 0; t < n; t++) {
        const float *x = X(t);
        double sq = 0;

        for (uint32_t i = 0; i < E; i++) sq += (double)x[i] * x[i];
        m->passage[t].layer[l].stream = (float)sqrt(sq);
      }
    }
    lap(m, &since, LLM_STAGE_ADD);
  }

  /* Only the last token's logits: what follows the batch. */
  norm(m, X(n - 1), XN(n - 1), m->out_norm_w, m->out_norm_b);
  TRACE("result_norm", 0, XN(n - 1), E);
  lap(m, &since, LLM_STAGE_NORM);
  matmul(m, m->out_w, m->out_b, XN(n - 1), E, m->logits, 0, 1, 0);
  lap(m, &since, LLM_STAGE_PRODUCTS);
  TRACE("result_output", 0, m->logits, m->n_vocab);

#undef X
#undef XN
#undef QKV
#undef AOUT
#undef FF
#undef ATTN
#undef UP

  m->n_past += n;
  return 0;
}

void llm_watch(struct llm_model *m, struct llm_passage *into)
{
  m->passage = into;
}

void llm_candidates(const struct llm_model *m, struct llm_passage *p)
{
  const float *z = m->logits;
  float max = z[0];
  double sum = 0;
  uint32_t n = 0;

  for (uint32_t i = 1; i < m->n_vocab; i++)
    if (z[i] > max) max = z[i];

  for (uint32_t i = 0; i < m->n_vocab; i++) sum += exp((double)(z[i] - max));

  /* The ten largest, kept in order as they are met: a tie keeps the
   * lower id first, as `llm_greedy` chooses it. */
  for (uint32_t i = 0; i < m->n_vocab; i++) {
    uint32_t at = n;

    while (at > 0 && z[i] > z[p->cand[at - 1]]) at--;
    if (at >= LLM_PASSAGE_CANDIDATES) continue;

    if (n < LLM_PASSAGE_CANDIDATES) n++;
    for (uint32_t k = n - 1; k > at; k--) p->cand[k] = p->cand[k - 1];
    p->cand[at] = (int32_t)i;
  }

  for (uint32_t k = 0; k < n; k++) p->odds[k] = (float)(exp((double)(z[p->cand[k]] - max)) / sum);
  p->n_cand = n;
}

int32_t llm_greedy(const struct llm_model *m)
{
  int32_t best = 0;
  for (uint32_t i = 1; i < m->n_vocab; i++)
    if (m->logits[i] > m->logits[best]) best = (int32_t)i;
  return best;
}
