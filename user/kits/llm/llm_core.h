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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "llm_dot.h"

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

/*
 * **Every core** (`docs/xavier.md`, *Making it fast*): the caller's way to
 * run `parts` pieces of a job at once - `fn(job, part, parts)` for each
 * part from 0, on as many cores as it has - and return when all are done.
 * The kit cuts a product's rows into the parts and nothing else: each row
 * is the same arithmetic wherever it runs, so the answer is the one a
 * single thread gives, to the bit. Given at `llm_parallel`; without it the
 * kit runs on the thread that calls it.
 */
typedef void llm_part_fn(void *job, uint32_t part, uint32_t parts);

/* A counter that only goes up, in whatever units its owner keeps
 * (`llm_clock`); the kit only ever subtracts two of its readings. */
typedef uint64_t llm_clock_fn(void);

/* The forward pass's stages, as `stage_counts` keeps them: the embedding
 * row; the layer norms; the products (the GPU's and the processor's,
 * waiting included); RoPE and the cache's writes; attention; GELU; and
 * the residual sums. */
#define LLM_STAGE_EMBED     0u
#define LLM_STAGE_NORM      1u
#define LLM_STAGE_PRODUCTS  2u
#define LLM_STAGE_ROPE      3u
#define LLM_STAGE_ATTENTION 4u
#define LLM_STAGE_GELU      5u
#define LLM_STAGE_ADD       6u
#define LLM_STAGES          7u
#define LLM_STAGES_MAX      8u

/* Attention's limits: a head no wider than this, and the parts it is cut
 * into no more than this, each with a row of `att` of its own. More parts
 * than that, and attention stays on the calling thread. */
#define LLM_HEAD_DIM_MAX    256u
#define LLM_ATTENTION_PARTS 64u
typedef void llm_run_fn(void *ctx, llm_part_fn *fn, void *job, uint32_t parts);

/*
 * **A product, offered** (`docs/xavier.md` X7): `out = W x + b` for `n`
 * inputs, the inputs already in Q8_0 - input `t`'s blocks at `xq + t *
 * xq_room` bytes and their scales at `xs + t * xs_room` floats - and the
 * answer for input `t`'s row `r` into `out[t * out_room + r]`, `bias` added
 * where there is one.
 *
 * **Shared** (X7 G5): it answers how many of the first rows it has begun -
 * every one, some, or, less than nought, none - and the processor's rows
 * do the rest meanwhile. Then `llm_product_done_fn` finishes the rows it
 * began: nought, or less when it could not, and the processor does those
 * as well. Every row's sum is the same whoever makes it.
 */
struct llm_tensor;
typedef int64_t llm_product_fn(void *ctx, const struct llm_tensor *w, const uint8_t *xq, size_t xq_room,
                               const float *xs, size_t xs_room, uint32_t n, const float *bias,
                               float *out, size_t out_room);
typedef int llm_product_done_fn(void *ctx);

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
  float *xs;                   /* and each block's scale, as a float */
  /* Each working vector but `att` and `logits` is `LLM_BATCH` of them, one
   * a token of a batch, the next `*_room` along (`llm_step_batch`). */
  uint32_t xq_room, xs_room;   /* bytes, and floats, a token's */

  /* The loops a row of each kind is multiplied by, chosen at `llm_open`
   * for the machine it runs on (`docs/xavier.md` X2): today the plain
   * loops, the fastest wherever they have been measured, until the M700
   * says whether the explicit lanes beat them there. */
  llm_dot_fn *dot_q8_0, *dot_q4_0;
  llm_dot4_fn *dot4_q8_0, *dot4_q4_0;   /* a row against four inputs, or NULL */
  unsigned loops;                       /* which they are: LLM_KIT_LOOPS_*  */

  /* How a product is run in parts, or NULL for this thread alone. */
  llm_run_fn *run;
  void *run_ctx;
  uint32_t parts;

  /* A product taken somewhere else - the GPU (`llm_gpu.h`) - or NULL. */
  llm_product_fn *product;
  llm_product_done_fn *product_done;
  void *product_ctx;

  /* Where each token's passage is recorded, `LLM_BATCH` of them, or NULL
   * (`llm_watch`). */
  struct llm_passage *passage;

  /* **Where the time goes** (`llm_clock`): the forward pass's stages, in
   * the clock's own counts, added up from the moment a clock was given.
   * No clock, and nothing is counted. */
  llm_clock_fn *clock;
  uint64_t stage_counts[LLM_STAGES_MAX];
};

/*
 * **A token's passage through the model** (`docs/xavier.md`, *The LLM
 * Monitor: Inside*): what the LLM Monitor's Inside page shows, recorded by
 * the forward pass from values it computes anyway, and only when it has
 * been handed somewhere to put them (`llm_watch`). Nothing here changes
 * the arithmetic: a model watched gives the answer it gives unwatched, to
 * the bit.
 *
 * For each layer: the residual stream's length after it - the token's
 * running meaning, which every layer adds to; how many of the
 * feed-forward's neurons fired, their input above nought; and for each
 * head, where its attention was strongest - how many tokens back from this
 * one (0, itself) - and what share of its attention that one position
 * had, of 65535. A model of more layers or heads than there is room for is
 * recorded up to the room, and says how many it has.
 */
#define LLM_PASSAGE_LAYERS     64u
#define LLM_PASSAGE_HEADS      32u
#define LLM_PASSAGE_CANDIDATES 10u

struct llm_passage_layer {
  float    stream;
  uint32_t fired;
  uint16_t back[LLM_PASSAGE_HEADS];
  uint16_t share[LLM_PASSAGE_HEADS];
};

struct llm_passage {
  uint32_t pos;                 /* the token's position in the cache      */
  int32_t  token;
  uint32_t n_layer, n_head, n_ff;   /* the model's, however many are here */
  uint32_t n_cand;              /* candidates below, by `llm_candidates`  */
  int32_t  cand[LLM_PASSAGE_CANDIDATES];  /* the likeliest next, likeliest first */
  float    odds[LLM_PASSAGE_CANDIDATES];  /* each one's probability       */
  struct llm_passage_layer layer[LLM_PASSAGE_LAYERS];
};

_Static_assert(sizeof(struct llm_passage_layer) == 136, "a layer's record is 136 bytes");
_Static_assert(sizeof(struct llm_passage) == 104 + 136 * LLM_PASSAGE_LAYERS, "a passage is as llmring.h says");

/* From the next step on, token `t` of a batch recorded into `into[t]` -
 * `LLM_BATCH` of them - or, with NULL, nothing recorded. */
void llm_watch(struct llm_model *m, struct llm_passage *into);

/* The likeliest tokens to follow, from `m->logits`, into `p`: their
 * probabilities a softmax over the whole vocabulary, as the choice of the
 * next token sees them. */
void llm_candidates(const struct llm_model *m, struct llm_passage *p);

/* The forward pass's stages timed with `clock` from now on, into
 * `m->stage_counts`; NULL for none. */
void llm_clock(struct llm_model *m, llm_clock_fn *clock);

/* Products in `parts` pieces through `run` from now on; NULL, or 1 part,
 * for the calling thread alone. */
void llm_parallel(struct llm_model *m, llm_run_fn *run, void *ctx, uint32_t parts);

/* Products offered to `product` first from now on (`docs/xavier.md` X7):
 * it begins some of a product's rows, the processor's do the rest, and
 * `done` finishes the ones it began. NULL for the processor alone. */
void llm_offload(struct llm_model *m, llm_product_fn *product, llm_product_done_fn *done, void *ctx);

/*
 * **Which loops multiply the weights** (`docs/xavier.md`, *Settings*). Every
 * set gives the same answer to the bit; only the speed differs. PLAIN is the
 * scalar reference, one input at a time; BASE the vectors every core of the
 * kind has, SSE2 or NEON, and the four-input loops; AVX2 the same, 32 bytes
 * wide. AUTO is the fastest this processor has.
 */
#define LLM_KIT_LOOPS_AUTO   0u
#define LLM_KIT_LOOPS_PLAIN  1u
#define LLM_KIT_LOOPS_BASE   2u
#define LLM_KIT_LOOPS_AVX2   3u

/* Bit 1 << LLM_KIT_LOOPS_x for each set this processor can run. */
unsigned llm_loops_have(void);

/* `which` from now on, AUTO resolved: the set chosen, or -1 when this
 * processor cannot run it, and the loops are left as they were. */
int llm_use_loops(struct llm_model *m, unsigned which);

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

/*
 * **`n` tokens through the model together**, at the next `n` positions -
 * up to `LLM_BATCH` of them; the logits are the last one's (`docs/xavier.md`,
 * *Making it fast*). The answer is the one `n` calls of `llm_step` give, to
 * the bit: every token meets the same arithmetic in the same order. What
 * changes is how often a weight is read. Writing an answer is bound by
 * memory, since every token reads every weight; a question's tokens are all
 * known at once, so a row of a weight is read once and met by every token
 * of the batch while it is in the cache. -1 as `llm_step`, and then nothing
 * of the batch is read.
 */
#define LLM_BATCH 32

int llm_step_batch(struct llm_model *m, const int32_t *tokens, uint32_t n);

/* Forget the conversation: the next step is position 0. */
void llm_reset(struct llm_model *m);

/* The most likely next token, from `m->logits`. */
int32_t llm_greedy(const struct llm_model *m);

/*
 * **What the cache holds, as tokens and as the text they stand for**
 * (`docs/xavier.md` X4). A conversation is asked again with every turn,
 * and reading it all again would be the whole cost of a follow-up: phi-2
 * reads a token about as slowly as it writes one. So whoever holds the
 * model keeps this beside it, and `llm_prompt` keeps what is already in the
 * cache and reads only what is new.
 *
 * The caller gives the room: `tokens` for `n_ctx` of them and `text` for
 * `text_room` bytes. `n` is how many are held and must equal the model's
 * `n_past`; a model stepped other than through `llm_feed` no longer agrees,
 * and is then taken as holding nothing. `text_bytes` is `LLM_TEXT_UNKNOWN`
 * when the text outgrew its room, and then only the tokens are compared.
 */
#define LLM_TEXT_UNKNOWN 0xffffffffu

struct llm_held {
  int32_t *tokens;
  uint32_t n;
  char    *text;
  uint32_t text_room;
  uint32_t text_bytes;
};

/* Nothing held: the next prompt is read from its first token. */
void llm_held_reset(struct llm_model *m, struct llm_held *h);

/*
 * Ready the cache for `text` of `n` bytes, the whole conversation so far:
 * the tokens still to be read into `out` of `room`, and how many there are
 * - at least one, since an answer begins from the last token read - or -1
 * when `text` is more than the cache holds. `*kept` is how many of the
 * held tokens were kept.
 *
 * Two ways to keep, and the first is tried first. When the held text is
 * where `text` begins, only the rest is tokenized, after what is held - a
 * follow-up reads its own words and nothing else. Otherwise `text` is
 * tokenized whole and the cache rewound to where its tokens and the held
 * ones first differ; a token at the same position gives the same cache
 * entry, so the answer is the one a fresh read would give.
 */
int llm_prompt(struct llm_model *m, struct llm_held *h, const char *text, size_t n,
               int32_t *out, int room, uint32_t *kept);

/* One token through the model, as `llm_step`, and held. */
int llm_feed(struct llm_model *m, struct llm_held *h, int32_t token);

/* `n` tokens through the model, as `llm_step_batch`, and held. */
int llm_feed_batch(struct llm_model *m, struct llm_held *h, const int32_t *tokens, uint32_t n);

/* IEEE half precision, as ggml converts it: to nearest, ties to even. */
uint16_t llm_f32_to_f16(float f);

/* GELU as the kit takes it in a layer, through half precision as ggml's
 * table: for the tests, which hold the table to the formula. */
float llm_gelu(float x);

/* And back, exactly. Here rather than in `llm_core.c` because it is in the
 * innermost loops - two a block of every row - where a call, or a branch-
 * free conversion of every case, cost more than the block's arithmetic
 * (`docs/xavier.md` X2). A normal number, which every real scale is, is
 * three operations; zero, a subnormal, infinity and NaN take the branch. */
static inline float llm_f16_to_f32(uint16_t h)
{
  uint32_t sign = (uint32_t)(h & 0x8000) << 16;
  uint32_t exp = (h >> 10) & 0x1f;
  uint32_t mant = h & 0x3ff;
  uint32_t bits;

  if (__builtin_expect(exp != 0 && exp != 0x1f, 1)) {
    bits = sign | (((uint32_t)(h & 0x7fff) << 13) + ((127 - 15) << 23));
  } else if (exp == 0x1f) {
    bits = sign | 0x7f800000 | (mant << 13);
  } else if (mant == 0) {
    bits = sign;
  } else {
    while (!(mant & 0x400)) { mant <<= 1; exp--; }  /* normalise a subnormal */
    exp++;
    mant &= 0x3ff;
    bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
  }

  float f;
  __builtin_memcpy(&f, &bits, sizeof f);
  return f;
}

#endif
