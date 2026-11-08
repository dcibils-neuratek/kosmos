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

  m->bos = (int32_t)llm_meta_number(m, "tokenizer.ggml.bos_token_id", -1);
  m->eos = (int32_t)llm_meta_number(m, "tokenizer.ggml.eos_token_id", -1);
  return 0;

no_memory:
  say(why, room, "no memory for the tokenizer's tables");
  return -1;
}

int llm_open(struct llm_model *m, const void *file, size_t size, uint32_t n_ctx,
             llm_alloc_fn alloc, void *ctx, char *why, size_t room)
{
  memset(m, 0, sizeof *m);
  m->file = file;
  m->size = size;

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

  if (!str_is(m->arch, "phi2")) {
    say(why, room, "an architecture this does not run yet (phi2 is the one)");
    return -1;
  }

  m->n_embd = (uint32_t)arch_number(m, "embedding_length", 0);
  m->n_head = (uint32_t)arch_number(m, "attention.head_count", 0);
  m->n_head_kv = (uint32_t)arch_number(m, "attention.head_count_kv", m->n_head);
  m->n_layer = (uint32_t)arch_number(m, "block_count", 0);
  m->n_ff = (uint32_t)arch_number(m, "feed_forward_length", 0);
  m->n_ctx_train = (uint32_t)arch_number(m, "context_length", 2048);
  m->norm_eps = (float)arch_number(m, "attention.layer_norm_epsilon", 1e-5);
  m->rope_base = (float)arch_number(m, "rope.freq_base", 10000.0);

  m->tok_embd = tensor(m, "token_embd.weight");
  m->n_vocab = m->tok_embd && m->tok_embd->n_dims == 2 ? (uint32_t)m->tok_embd->ne[1] : 0;

  if (!m->n_embd || !m->n_head || m->n_embd % m->n_head || m->n_head_kv != m->n_head
      || !m->n_layer || m->n_layer > 1024 || !m->n_ff || m->n_ff > (1u << 20)
      || m->n_embd > (1u << 16) || !m->n_vocab || m->n_vocab > (1u << 20)) {
    say(why, room, "its shape is missing, or not one phi2 has");
    return -1;
  }

  m->head_dim = m->n_embd / m->n_head;
  m->n_rot = (uint32_t)arch_number(m, "rope.dimension_count", m->head_dim);
  if (m->n_rot > m->head_dim || (m->n_rot & 1)) { say(why, room, "a bad rotary size"); return -1; }

  uint32_t E = m->n_embd;

  m->out_norm_w = tensor(m, "output_norm.weight");
  m->out_norm_b = tensor(m, "output_norm.bias");
  m->out_w = tensor(m, "output.weight");
  m->out_b = tensor(m, "output.bias");

  if (!is_matrix(m->tok_embd, E, m->n_vocab) || m->tok_embd->type == LLM_F32
      || !is_vector(m->out_norm_w, E) || !is_vector(m->out_norm_b, E)
      || !is_matrix(m->out_w, E, m->n_vocab) || !is_vector(m->out_b, m->n_vocab)) {
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

    int fused = is_matrix(L->qkv_w, E, 3 * E) && is_vector(L->qkv_b, 3 * E);
    int apart = is_matrix(L->q_w, E, E) && is_vector(L->q_b, E)
                && is_matrix(L->k_w, E, E) && is_vector(L->k_b, E)
                && is_matrix(L->v_w, E, E) && is_vector(L->v_b, E);

    if (!is_vector(L->norm_w, E) || !is_vector(L->norm_b, E) || !(fused || apart)
        || !is_matrix(L->o_w, E, E) || !is_vector(L->o_b, E)
        || !is_matrix(L->up_w, E, m->n_ff) || !is_vector(L->up_b, m->n_ff)
        || !is_matrix(L->down_w, m->n_ff, E) || !is_vector(L->down_b, E)) {
      say(why, room, "a layer's tensors are missing or the wrong shape");
      return -1;
    }
    if (!fused) L->qkv_w = L->qkv_b = NULL;
  }

  if (open_tokenizer(m, alloc, ctx, why, room) < 0) return -1;

  m->n_ctx = n_ctx ? n_ctx : m->n_ctx_train;
  if (m->n_ctx > m->n_ctx_train) m->n_ctx = m->n_ctx_train;

  size_t cache = (size_t)m->n_layer * m->n_ctx * E;
  uint32_t widest = m->n_ff > 3 * E ? m->n_ff : 3 * E;

  m->k_cache = alloc(ctx, cache * 2);
  m->v_cache = alloc(ctx, cache * 2);
  m->x = alloc(ctx, E * 4);
  m->xn = alloc(ctx, E * 4);
  m->qkv = alloc(ctx, 3 * E * 4);
  m->att = alloc(ctx, (size_t)m->n_ctx * 4);
  m->attn_out = alloc(ctx, E * 4);
  m->ff = alloc(ctx, (size_t)m->n_ff * 4);
  m->ff_out = alloc(ctx, E * 4);
  m->logits = alloc(ctx, (size_t)m->n_vocab * 4);
  m->xq = alloc(ctx, (size_t)widest / LLM_QK * 34 + 34);
  m->xs = alloc(ctx, ((size_t)widest / LLM_QK + 1) * 4);

  if (!m->k_cache || !m->v_cache || !m->x || !m->xn || !m->qkv || !m->att || !m->attn_out
      || !m->ff || !m->ff_out || !m->logits || !m->xq || !m->xs) {
    say(why, room, "no memory for its attention cache and working vectors");
    return -1;
  }

  m->dot_q8_0 = llm_dot_q8_0_scalar;
  m->dot_q4_0 = llm_dot_q4_0_scalar;

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

/* A run of text with no special token in it: GPT-2's split, then BPE. */
static void tokenize_plain(const struct llm_model *m, const char *s, size_t n, struct out *o)
{
  /* The text as characters, with each one's place in the bytes. */
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
    {
      /* The word as GPT-2's characters: each byte of its UTF-8 mapped. */
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

/* One row of a weight times the input, as ggml multiplies it. */
static float row_dot(const struct llm_model *m, const struct llm_tensor *w, uint64_t row,
                     const float *x, const uint8_t *xq)
{
  uint64_t cols = w->ne[0];

  switch (w->type) {
    case LLM_Q8_0:
      return m->dot_q8_0(w->data + row * (cols / LLM_QK) * 34, xq, m->xs, cols / LLM_QK);
    case LLM_Q4_0:
      return m->dot_q4_0(w->data + row * (cols / LLM_QK) * 18, xq, m->xs, cols / LLM_QK);
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

/* out = W x + b, the rows from `first`, `rows` of them. */
static void matvec(struct llm_model *m, const struct llm_tensor *w, const struct llm_tensor *bias,
                   const float *x, uint8_t *xq, float *out, uint64_t first, uint64_t rows,
                   int quantised)
{
  if (!quantised && (w->type == LLM_Q8_0 || w->type == LLM_Q4_0))
    quantize_q8_0(x, xq, m->xs, (uint32_t)w->ne[0]);

  const float *b = bias ? (const float *)(const void *)bias->data : NULL;
  for (uint64_t r = 0; r < rows; r++) {
    float v = row_dot(m, w, first + r, x, xq);
    out[r] = b ? v + b[first + r] : v;
  }
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

/* GELU through half precision, as ggml's table: the input to F16, GELU of
 * that, the answer to F16. */
static float gelu(float x)
{
  if (x <= -10.0f) return 0.0f;
  if (x >= 10.0f) return x;

  float h = llm_f16_to_f32(llm_f32_to_f16(x));
  float g = 0.5f * h * (1.0f + (float)tanh((double)(0.79788456080286535587989211986876f * h
                                                      * (1.0f + 0.044715f * h * h))));
  return llm_f16_to_f32(llm_f32_to_f16(g));
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
  char piece[256];
  int b;

  if (h->n != m->n_past) llm_held_reset(m, h);

  if (llm_step(m, token) != 0) return -1;

  h->tokens[h->n++] = token;

  if (h->text_bytes != LLM_TEXT_UNKNOWN) {
    b = llm_token_text(m, token, piece, (int)sizeof piece);

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
  if (token < 0 || (uint32_t)token >= m->n_vocab || m->n_past >= m->n_ctx) return -1;

  const uint32_t E = m->n_embd, D = m->head_dim, pos = m->n_past;
  float *x = m->x, *xn = m->xn, *q = m->qkv, *k = m->qkv + E, *v = m->qkv + 2 * E;

  embedding_row(m->tok_embd, (uint32_t)token, x);

  for (uint32_t l = 0; l < m->n_layer; l++) {
    const struct llm_layer *L = &m->layers[l];

    layer_norm(x, xn, E, L->norm_w, L->norm_b, m->norm_eps);
    TRACE("attn_norm", l, xn, E);

    /* Q, K and V: one product, or three over the same quantised input. */
    if (L->qkv_w) {
      matvec(m, L->qkv_w, L->qkv_b, xn, m->xq, m->qkv, 0, 3 * E, 0);
    } else {
      matvec(m, L->q_w, L->q_b, xn, m->xq, q, 0, E, 0);
      matvec(m, L->k_w, L->k_b, xn, m->xq, k, 0, E, L->k_w->type == L->q_w->type);
      matvec(m, L->v_w, L->v_b, xn, m->xq, v, 0, E, L->v_w->type == L->q_w->type);
    }

    TRACE("Qcur", l, q, E);
    TRACE("Kcur", l, k, E);
    TRACE("Vcur", l, v, E);

    float qscale = 1.0f / sqrtf((float)D);

    for (uint32_t h = 0; h < m->n_head; h++) {
      rope(q + h * D, m->n_rot, pos, m->rope_base);
      rope(k + h * D, m->n_rot, pos, m->rope_base);
      for (uint32_t i = 0; i < D; i++) q[h * D + i] *= qscale;
    }

    uint16_t *kc = m->k_cache + ((size_t)l * m->n_ctx) * E;
    uint16_t *vc = m->v_cache + ((size_t)l * m->n_ctx) * E;

    for (uint32_t i = 0; i < E; i++) {
      kc[(size_t)pos * E + i] = llm_f32_to_f16(k[i]);
      vc[(size_t)pos * E + i] = llm_f32_to_f16(v[i]);
    }

    /* Attention, a head at a time, over every position so far. The query
     * meets the F16 cache as F16, and so do the probabilities. */
    for (uint32_t h = 0; h < m->n_head; h++) {
      float *qh = q + h * D, *att = m->att;
      float max = -INFINITY;

      for (uint32_t t = 0; t <= pos; t++) {
        const uint16_t *kt = kc + (size_t)t * E + h * D;
        double s = 0;
        for (uint32_t i = 0; i < D; i++)
          s += (double)(llm_f16_to_f32(llm_f32_to_f16(qh[i])) * llm_f16_to_f32(kt[i]));
        att[t] = (float)s;
        if (att[t] > max) max = att[t];
      }

      double sum = 0;
      for (uint32_t t = 0; t <= pos; t++) {
        att[t] = (float)exp((double)(att[t] - max));
        sum += (double)att[t];
      }
      float inv = (float)(1.0 / sum);

      for (uint32_t t = 0; t <= pos; t++) att[t] = llm_f16_to_f32(llm_f32_to_f16(att[t] * inv));

      float *o = m->attn_out + h * D;
      for (uint32_t i = 0; i < D; i++) {
        double s = 0;
        for (uint32_t t = 0; t <= pos; t++) s += (double)(att[t] * llm_f16_to_f32(vc[(size_t)t * E + h * D + i]));
        o[i] = (float)s;
      }
    }

    /* The attention's output, and the MLP beside it off the same norm,
     * which `xn` still holds: phi-2's layer is the two in parallel. Q, K
     * and V are done with, so the MLP's output borrows their room. */
    float *attn = m->ff_out, *mlp = m->qkv;

    TRACE("kqv_out", l, m->attn_out, E);
    matvec(m, L->o_w, L->o_b, m->attn_out, m->xq, attn, 0, E, 0);
    TRACE("attn_out", l, attn, E);
    matvec(m, L->up_w, L->up_b, xn, m->xq, m->ff, 0, m->n_ff, 0);
    TRACE("ffn_up_b", l, m->ff, m->n_ff);
    for (uint32_t i = 0; i < m->n_ff; i++) m->ff[i] = gelu(m->ff[i]);
    TRACE("ffn_gelu", l, m->ff, m->n_ff);
    matvec(m, L->down_w, L->down_b, m->ff, m->xq, mlp, 0, E, 0);
    TRACE("ffn_out", l, mlp, E);

    /* ggml's order: the two added, then the residual. */
    for (uint32_t i = 0; i < E; i++) x[i] = (attn[i] + mlp[i]) + x[i];
    TRACE("l_out", l, x, E);
  }

  layer_norm(x, xn, E, m->out_norm_w, m->out_norm_b, m->norm_eps);
  TRACE("result_norm", 0, xn, E);
  matvec(m, m->out_w, m->out_b, xn, m->xq, m->logits, 0, m->n_vocab, 0);
  TRACE("result_output", 0, m->logits, m->n_vocab);

  m->n_past++;
  return 0;
}

int32_t llm_greedy(const struct llm_model *m)
{
  int32_t best = 0;
  for (uint32_t i = 1; i < m->n_vocab; i++)
    if (m->logits[i] > m->logits[best]) best = (int32_t)i;
  return best;
}
