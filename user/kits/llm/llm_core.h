/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The LLM Kit's core** (`docs/xavier.md`, step X1): a language model read
 * from a GGUF file and run, in C that compiles on the Mac and on Kosmos
 * alike - the same file, held on the Mac to llama.cpp itself by
 * `tools/test_llm.c`.
 *
 * What it reads is what llama.cpp writes: GGUF version 2 and 3, tensors in
 * F32, F16, Q8_0 and Q4_0, ggml's block layouts. What it runs is phi-2
 * (`general.architecture = phi2`); a second architecture is a second
 * forward pass beside it.
 *
 * **It takes no memory of its own.** The file is a buffer the caller
 * holds - a region on Kosmos, a malloc on the Mac - and is read in place,
 * never copied: the weights are used where they lie. Everything else - the
 * tokenizer's tables, the attention cache, the working vectors - comes from
 * `alloc`, once, at `llm_open`, so a running model allocates nothing.
 *
 * Nothing here trusts the file: every length, count, offset and type is
 * held to the buffer before it is used, and a file that lies is refused
 * with a sentence rather than read past.
 */

#ifndef KOSMOS_LLM_CORE_H
#define KOSMOS_LLM_CORE_H

#include <stddef.h>
#include <stdint.h>

/* ggml's tensor types, the four this reads. */
enum {
  LLM_F32  = 0,
  LLM_F16  = 1,
  LLM_Q4_0 = 2,
  LLM_Q8_0 = 8,
};

#define LLM_QK 32              /* values in a Q4_0 or Q8_0 block */
#define LLM_MAX_DIMS 4

/* A byte string inside the file: not terminated. */
struct llm_str {
  const char *s;
  uint32_t n;
};

struct llm_tensor {
  struct llm_str name;
  uint32_t type;
  uint32_t n_dims;
  uint64_t ne[LLM_MAX_DIMS];   /* ne[0] is a row's length, as ggml's */
  const uint8_t *data;
  uint64_t bytes;
};

/* One layer of phi-2. */
struct llm_layer {
  const struct llm_tensor *norm_w, *norm_b;
  const struct llm_tensor *qkv_w, *qkv_b;          /* fused, or ...   */
  const struct llm_tensor *q_w, *q_b, *k_w, *k_b, *v_w, *v_b;
  const struct llm_tensor *o_w, *o_b;
  const struct llm_tensor *up_w, *up_b, *down_w, *down_b;
};

/* Where `alloc` gets memory from: the caller's. Returns zeroed memory or
 * NULL. */
typedef void *(*llm_alloc_fn)(void *ctx, size_t bytes);

struct llm_model {
  /* The file. */
  const uint8_t *file;
  size_t size;
  uint32_t version;
  uint32_t n_tensors;
  struct llm_tensor *tensors;
  void *meta;                  /* the key-value pairs, where they lie */
  uint32_t n_meta;
  struct llm_str arch;
  struct llm_str name;

  /* The shape. */
  uint32_t n_vocab, n_embd, n_head, n_head_kv, n_layer, n_ff, n_rot, n_ctx_train;
  uint32_t head_dim;
  float norm_eps, rope_base;

  /* The weights. */
  const struct llm_tensor *tok_embd, *out_norm_w, *out_norm_b, *out_w, *out_b;
  struct llm_layer *layers;

  /* The tokenizer. */
  struct llm_str *tokens;      /* by id, in GPT-2's byte-encoded form  */
  int32_t *token_type;         /* 1 normal, 3 control, 4 user-defined  */
  uint32_t *token_hash;        /* open addressing: text -> id + 1      */
  uint32_t token_hash_n;
  struct llm_str *merges;      /* "left right", by rank                */
  uint32_t n_merges;
  uint32_t *merge_hash;        /* "left right" -> rank + 1             */
  uint32_t merge_hash_n;
  uint32_t *specials;          /* user-defined then control ids, longest first */
  uint32_t n_specials;
  int32_t bos, eos;

  /* The running state: the attention cache, as F16, and the working
   * vectors - all from `alloc`, at `llm_open`. */
  uint32_t n_ctx;              /* positions the cache holds */
  uint32_t n_past;             /* positions it holds now    */
  uint16_t *k_cache, *v_cache; /* [layer][pos][n_embd]      */
  float *x, *xn, *qkv, *att, *attn_out, *ff, *ff_out, *logits;
  uint8_t *xq;                 /* the input, as Q8_0 blocks */
};

/*
 * The model in `file`, of `size` bytes, for a cache of `n_ctx` positions
 * (0: the model's own, up to 2048). 0 on success; otherwise -1, with why
 * in `why`.
 */
int llm_open(struct llm_model *m, const void *file, size_t size, uint32_t n_ctx,
             llm_alloc_fn alloc, void *alloc_ctx, char *why, size_t why_room);

/* A key's value from the file's metadata, as a number, or `fallback`. */
double llm_meta_number(const struct llm_model *m, const char *key, double fallback);

/*
 * `text` of `n` bytes as tokens into `out` of `room`: how many were
 * written, or -1 if they did not fit. With `special`, a control token's
 * text (`<|endoftext|>`) is that token rather than its letters, as
 * llama.cpp's `parse_special`.
 */
int llm_tokenize(const struct llm_model *m, const char *text, size_t n, int special,
                 int32_t *out, int room);

/* A token's text as the bytes it stands for, into `out` of `room`: how
 * many bytes. */
int llm_token_text(const struct llm_model *m, int32_t token, char *out, int room);

/*
 * One token through the model, at the next position: the logits for what
 * follows it are then in `m->logits`, `m->n_vocab` of them. -1 when the
 * cache is full or the token is not one.
 */
int llm_step(struct llm_model *m, int32_t token);

/* Forget the conversation: the next step is position 0. */
void llm_reset(struct llm_model *m);

/* The most likely next token, from `m->logits`. */
int32_t llm_greedy(const struct llm_model *m);

/* IEEE half precision, as ggml converts it: to nearest, ties to even. */
uint16_t llm_f32_to_f16(float f);
float llm_f16_to_f32(uint16_t h);

#endif
