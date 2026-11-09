/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The LLM Kit held to llama.cpp** (`docs/xavier.md` X1), on the Mac.
 *
 * `tools/llm_reference.cpp` ran llama.cpp itself over a model and wrote
 * down what it said (`tools/llm_reference_<model>.txt`). This reads the
 * same model with the kit's `llm_core.c` and checks, for every case there:
 *
 *   - the tokens of each text, plain and with special tokens parsed - the
 *     same ids, in the same order;
 *   - a prompt of one token: the ten largest logits after it within
 *     EXACT of llama.cpp's - the arithmetic alone, before any drift;
 *   - each longer prompt fed a token at a time: its top token llama.cpp's
 *     or a near-tie, and the ten largest logits within TOLERANCE;
 *   - the answer, fed llama.cpp's own tokens so the two cannot wander
 *     apart: at each step the kit's next token is llama.cpp's, or both
 *     rate the other's choice within NEAR of their best - a near-tie, which
 *     llama.cpp built two ways decides both ways too.
 *
 * **Why not the same answer token for token.** llama.cpp does not give it
 * against itself. Built plain and built for this Mac's NEON, on these very
 * prompts (8 November 2026): one token's logits 0.052 apart, a seven-token
 * prompt's 0.62, a twelve-token prompt's top token chosen differently at a
 * near-tie (14.69 against 14.66), and answers that part at near-ties. The
 * order a sum is taken in moves a Q8_0 rounding, and that grows layer by
 * layer. The kit is a third order, and lands as far from either build as
 * they are from each other: 0.056 and 0.82 from the plain one, 0.052 and
 * 1.36 from NEON's. EXACT and TOLERANCE are about twice llama.cpp's own
 * spread. The controls, each a copy of the kit with one mistake in it:
 * GELU without its cubic term and the MLP's down bias left out are 1.37 and
 * 1.25 out on the first token, against EXACT's 0.10; BPE merging the
 * highest rank first fails every text but the empty one; and RoPE pairing
 * GPT-J's way rather than NeoX's fails two answer steps and no logit bound
 * - phi-2 turns 32 of a head's 80 numbers, nothing at position 0, and these
 * prompts are short, so that is the check that is thinnest.
 *
 * And it says how fast, which is the scalar loop's and the Mac's - a
 * reference, not a measure of anything on Kosmos.
 *
 * A model file that is not there is a skip that says so: they are built
 * into `build/models/` as `docs/xavier.md` X1 says, and are gigabytes.
 *
 * Usage: test_llm MODEL.gguf REFERENCE.txt
 */

#include "llm_core.h"
#include "llm_gpu.h"
#include "eu_emu.h"
#include "../user/include/gpuproto.h"

#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EXACT 0.10f
#define TOLERANCE 2.0f
#define NEAR 0.5f

/* The largest vocabulary the logit copies below hold: Qwen2's is 151936. */
#define VOCAB_MAX 262144

static int checks, failed;

static void check(int ok, const char *what)
{
  checks++;
  if (!ok) { failed++; printf("  FAIL: %s\n", what); }
}

/* The kit's GELU, a table of every half-precision input (18.729), held to
 * the formula it was before: every one of the 65536 inputs, and a spread of
 * floats either side of them, the same answer to the bit. */
static void check_gelu(void)
{
  uint32_t wrong = 0, first = 0;

  for (uint32_t i = 0; i < 65536u + 4000u; i++) {
    float x = i < 65536u ? llm_f16_to_f32((uint16_t)i) : -12.0f + (float)(i - 65536u) * 0.006f;
    float h, g, want, got = llm_gelu(x);

    if (x <= -10.0f) want = 0.0f;
    else if (x >= 10.0f) want = x;
    else {
      h = llm_f16_to_f32(llm_f32_to_f16(x));
      g = 0.5f * h * (1.0f + (float)tanh((double)(0.79788456080286535587989211986876f * h
                                                  * (1.0f + 0.044715f * h * h))));
      want = llm_f16_to_f32(llm_f32_to_f16(g));
    }

    if (memcmp(&got, &want, sizeof got) != 0) {
      if (wrong++ == 0) first = i;
    }
  }

  char what[120];
  snprintf(what, sizeof what, "GELU's table: %u of %u inputs not the formula's to the bit, the first %u",
           wrong, 65536u + 4000u, first);
  check(wrong == 0, what);
}

static void *alloc(void *ctx, size_t bytes)
{
  (void)ctx;
  return calloc(1, bytes ? bytes : 1);
}

/*
 * **The GPU, emulated** (`docs/xavier.md` X7 G3): `--gpu` offers every
 * product to the kit's GPU path, and the device behind it is the emulator
 * of the execution units (`eu_emu.c`) - the same kernel bytes the LLM
 * server hands the M700's driver. Then every check below is the GPU's, and
 * `check_gpu` holds its logits to the processor's to the bit.
 */
#define EMU_BUFFERS 600

static struct eu_buffer emu[EMU_BUFFERS];
static uint32_t emu_n = 1;                  /* nought is no buffer, as the driver's */
static const uint32_t *emu_kernel;
static uint32_t emu_kernel_bytes;
static uint64_t emu_instructions;

static const char *emu_take(uint8_t *bytes, size_t size, uint32_t *id)
{
  if (emu_n == EMU_BUFFERS) return "the emulator's buffers are full";

  emu[emu_n].bytes = bytes;
  emu[emu_n].width = 1024;
  emu[emu_n].pitch = 4096;
  emu[emu_n].height = (uint32_t)((size + 4095) / 4096);
  emu[emu_n].view = GPU_VIEW_BYTES;
  *id = emu_n++;
  return NULL;
}

static const char *emu_kernel_given(void *ctx, const uint8_t *code, size_t bytes, uint32_t *id)
{
  (void)ctx;
  emu_kernel = (const uint32_t *)(const void *)code;
  emu_kernel_bytes = (uint32_t)bytes;
  *id = 1;
  return NULL;
}

static const char *emu_buffer(void *ctx, size_t bytes, int read_back, uint32_t *id, void **cpu)
{
  uint8_t *b = calloc(1, (bytes + 4095) / 4096 * 4096);

  (void)ctx;
  (void)read_back;
  if (!b) return "no memory";
  *cpu = b;
  return emu_take(b, bytes, id);
}

static void emu_forget(void *ctx, uint32_t id)
{
  (void)ctx;
  free(emu[id].bytes);
  emu[id].bytes = NULL;
}

static const char *emu_run(void *ctx, uint32_t kernel, uint32_t simd, uint32_t groups,
                           const uint32_t *buffers, uint32_t nbuffers, const void *constants,
                           size_t nconstants)
{
  static uint8_t padded[GPU_RUN_CONSTANTS];
  struct eu_buffer bound[GPU_RUN_BUFFERS];
  struct eu_run r;
  struct eu_stats st;
  uint32_t at = 0;
  const char *why;

  (void)ctx;
  (void)kernel;
  memset(&r, 0, sizeof r);
  memset(padded, 0, sizeof padded);
  memcpy(padded, constants, nconstants);

  for (uint32_t i = 0; i < nbuffers; i++) bound[i] = emu[buffers[i]];

  r.kernel = emu_kernel;
  r.bytes = emu_kernel_bytes;
  r.simd = simd;
  r.groups_x = groups;
  r.groups_y = 1;
  r.buffers = bound;
  r.nbuffers = nbuffers;
  r.constants = padded;
  r.nconstants = (uint32_t)((nconstants + 31) & ~(size_t)31);
  r.most_steps = 2000000000u;

  if ((why = eu_emulate(&r, &st, &at)) != NULL) return why;

  emu_instructions += st.instructions;
  return NULL;
}

/* The emulator has finished by the time it answers; it says so every
 * other time, so the share is moved both ways. */
static const char *emu_wait(void *ctx, int *already)
{
  static unsigned calls;

  (void)ctx;
  *already = calls++ & 1;
  return NULL;
}

static struct llm_gpu gpu;
static int with_gpu;

/* The GPU's logits held to the processor's, to the bit, over a question of
 * more than one batch: fed a batch at a time, so the product takes many
 * inputs at once, and a token at a time after it. */
static void check_gpu(struct llm_model *m)
{
  static int32_t t[512];
  static float theirs[VOCAB_MAX];
  const char *q = "Instruct: Name three colours of the rainbow, and say why the sky is blue.\nOutput:";
  int n = llm_tokenize(m, q, strlen(q), 1, t, 512);
  uint64_t before = gpu.products;
  char what[200];

  if (n <= 0 || m->n_vocab > sizeof theirs / sizeof *theirs) {
    check(0, "gpu: the question would not tokenize");
    return;
  }

  /* On the GPU: the question as a batch, then two tokens after. */
  llm_reset(m);
  int ok = llm_step_batch(m, t, (uint32_t)n) == 0;
  ok = ok && llm_step(m, 13) == 0 && llm_step(m, 464) == 0;
  memcpy(theirs, m->logits, m->n_vocab * sizeof *theirs);
  uint64_t taken = gpu.products - before;

  /* And on the processor alone. */
  llm_offload(m, NULL, NULL, NULL);
  llm_reset(m);
  ok = ok && llm_step_batch(m, t, (uint32_t)n) == 0;
  ok = ok && llm_step(m, 13) == 0 && llm_step(m, 464) == 0;
  llm_offload(m, llm_gpu_product, llm_gpu_done, &gpu);

  int same = ok && memcmp(theirs, m->logits, m->n_vocab * sizeof *theirs) == 0;
  /* And shared: some rows on the GPU and the rest on the processor. */
  int shared = gpu.rows_gpu > 0 && gpu.rows_gpu < gpu.rows_all;

  snprintf(what, sizeof what, "gpu: %d tokens as a batch and two after, the GPU's logits the "
           "processor's to the bit (%llu products begun on the GPU, %llu of %llu rows there)", n,
           (unsigned long long)taken, (unsigned long long)gpu.rows_gpu, (unsigned long long)gpu.rows_all);
  check(same && taken > 0 && shared, what);
  printf("gpu: %llu products on the GPU, %llu left to the processor (%s), %llu instructions emulated\n",
         (unsigned long long)gpu.products, (unsigned long long)gpu.declined,
         gpu.declined ? gpu.why : "none", (unsigned long long)emu_instructions);
}

static double now(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

/* Hex text back to bytes. */
static size_t unhex(const char *h, char *out, size_t room)
{
  size_t n = 0;
  while (h[0] && h[1] && h[0] != ' ' && n < room) {
    unsigned v;
    sscanf(h, "%2x", &v);
    out[n++] = (char)v;
    h += 2;
  }
  return n;
}

static int numbers(const char *s, int *out, int room)
{
  int n = 0;
  while (*s && n < room) {
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s) { s++; continue; }
    out[n++] = (int)v;
    s = end;
  }
  return n;
}

/*
 * **A follow-up reads only what is new, and answers as a fresh read would**
 * (`docs/xavier.md` X4; `llm_core.h`, `struct llm_held`). The logits after
 * each way of keeping the cache are compared to the same tokens read from
 * nothing, to the bit: the same code in the same order, so anything but
 * equal is a cache entry kept that should not have been.
 */
static float fresh[VOCAB_MAX];

static int fresh_logits(struct llm_model *m, const int32_t *t, int n)
{
  llm_reset(m);
  for (int i = 0; i < n; i++) if (llm_step(m, t[i]) != 0) return -1;
  memcpy(fresh, m->logits, m->n_vocab * sizeof *fresh);
  return 0;
}

static int same_logits(const struct llm_model *m)
{
  return memcmp(fresh, m->logits, m->n_vocab * sizeof *fresh) == 0;
}

static void check_held(struct llm_model *m)
{
  static int32_t room[2048], held_tokens[2048], seq[2048];
  static char held_text[16384];
  struct llm_held h = { held_tokens, 0, held_text, sizeof held_text, 0 };
  const char *a = "Instruct: What is the capital of France?\nOutput:";
  const char *b = "Instruct: What is the capital of France?\nOutput: The capital of France is "
                  "Paris.\nInstruct: And of Spain?\nOutput:";
  const char *c = "Instruct: What is the capital of France?\nOutput: Something else entirely";
  uint32_t kept;
  int n, na, nseq, whole;
  char what[160];

  if (m->n_vocab > sizeof fresh / sizeof *fresh) {
    check(0, "the held context's check: more logits than its room");
    return;
  }

  /* The first question: nothing held, so all of it read. */
  llm_held_reset(m, &h);
  na = llm_prompt(m, &h, a, strlen(a), room, 2048, &kept);
  snprintf(what, sizeof what, "held: the first question read whole, %d tokens, %u kept", na, kept);
  check(na > 0 && kept == 0, what);
  for (int i = 0; i < na; i++) { llm_feed(m, &h, room[i]); seq[i] = room[i]; }
  check(h.n == (uint32_t)na && h.text_bytes == strlen(a), "held: its tokens and its text held");

  /* The follow-up: the held text is where it begins, so only the rest. */
  n = llm_prompt(m, &h, b, strlen(b), room, 2048, &kept);
  snprintf(what, sizeof what, "held: the follow-up reads only its own %d tokens, %u kept", n, kept);
  check(n > 0 && kept == (uint32_t)na, what);
  for (int i = 0; i < n; i++) { llm_feed(m, &h, room[i]); seq[na + i] = room[i]; }
  nseq = na + n;
  {
    static float after[VOCAB_MAX];
    memcpy(after, m->logits, m->n_vocab * sizeof *after);
    fresh_logits(m, seq, nseq);
    check(memcmp(after, fresh, m->n_vocab * sizeof *after) == 0,
          "held: the follow-up's logits are those of the same tokens read from nothing");
  }

  /* `fresh_logits` read the very tokens held, so the cache still agrees
   * with them. A different answer after the question: rewound to where the
   * tokens part, and the logits a fresh read's. */
  n = llm_prompt(m, &h, c, strlen(c), room, 2048, &kept);
  whole = llm_tokenize(m, c, strlen(c), 1, seq, 2048);
  snprintf(what, sizeof what, "held: another answer rewinds - %u of %d kept, %d read", kept,
           whole, n);
  check(n > 0 && kept >= (uint32_t)na - 1 && kept + (uint32_t)n == (uint32_t)whole, what);
  for (int i = 0; i < n; i++) llm_feed(m, &h, room[i]);
  {
    static float after[VOCAB_MAX];
    memcpy(after, m->logits, m->n_vocab * sizeof *after);
    fresh_logits(m, seq, whole);
    check(memcmp(after, fresh, m->n_vocab * sizeof *after) == 0,
          "held: after a rewind, the logits are a fresh read's to the bit");
  }

  /* Now a turn taken back: the text differs after the question, so the
   * cache is rewound to where the tokens part. */
  n = llm_prompt(m, &h, b, strlen(b), room, 2048, &kept);
  whole = llm_tokenize(m, b, strlen(b), 1, seq, 2048);
  snprintf(what, sizeof what, "held: back to the first answer rewinds - %u of %d kept, %d read", kept,
           whole, n);
  check(n > 0 && kept > 0 && kept + (uint32_t)n == (uint32_t)whole, what);
  for (int i = 0; i < n; i++) llm_feed(m, &h, room[i]);
  {
    static float after[VOCAB_MAX];
    memcpy(after, m->logits, m->n_vocab * sizeof *after);
    fresh_logits(m, seq, whole);
    check(memcmp(after, fresh, m->n_vocab * sizeof *after) == 0,
          "held: rewound again, the logits are a fresh read's to the bit");
  }

  /* And the same text twice: everything kept but the last token, read
   * again for the logits an answer begins from. */
  llm_held_reset(m, &h);
  n = llm_prompt(m, &h, b, strlen(b), room, 2048, &kept);
  for (int i = 0; i < n; i++) llm_feed(m, &h, room[i]);
  n = llm_prompt(m, &h, b, strlen(b), room, 2048, &kept);
  snprintf(what, sizeof what, "held: the same text again reads one token, %d, %u kept", n, kept);
  check(n == 1 && kept + 1 == (uint32_t)whole, what);
  llm_feed(m, &h, room[0]);
  check(same_logits(m), "held: and its logits are the fresh read's");

  printf("\nheld: a follow-up read %d tokens of %d; a rewind kept %u\n", whole - na, whole, kept);
}

/*
 * **A question read in batches is read as one token at a time would read
 * it** (`docs/xavier.md`, *Making it fast*). The same tokens fed one at a
 * time, then in batches: the logits must be the same to the bit, and so
 * must every position of the attention cache, since the answer that follows
 * reads it. The question is longer than one batch, so a batch boundary
 * falls inside it, and the second batch's tokens attend across it.
 */
static void check_batch(struct llm_model *m)
{
  static int32_t t[2048];
  static char text[4096];
  size_t at = 0;
  int n;
  char what[160];

  if (m->n_vocab > sizeof fresh / sizeof *fresh) {
    check(0, "the batch's check: more logits than its room");
    return;
  }

  /* Counted on until it is more than a batch: a tokenizer that takes
   * digits one at a time - Qwen2's - gets there sooner than phi-2's. */
  at += (size_t)snprintf(text + at, sizeof text - at, "Instruct: Count with me:");
  n = 0;
  for (int i = 1; i <= 40 && n <= LLM_BATCH + 8; i++) {
    at += (size_t)snprintf(text + at, sizeof text - at, " %d,", i);
    snprintf(text + at, sizeof text - at, " and then?\nOutput:");
    n = llm_tokenize(m, text, strlen(text), 1, t, 2048);
  }

  snprintf(what, sizeof what, "batch: the question is more than one batch, %d tokens", n);
  check(n > LLM_BATCH && n < 2 * LLM_BATCH, what);
  if (n <= LLM_BATCH || (uint32_t)n > m->n_ctx) return;

  double t0 = now();

  if (fresh_logits(m, t, n) != 0) {
    check(0, "batch: a token at a time would not read the question");
    return;
  }

  double one_at_a_time = now() - t0;

  size_t cache = (size_t)m->n_layer * m->n_ctx * m->kv_dim;
  size_t used = (size_t)n * m->kv_dim;
  uint16_t *k1 = malloc(cache * 2), *v1 = malloc(cache * 2);

  if (!k1 || !v1) { check(0, "batch: no memory to compare the cache"); free(k1); free(v1); return; }
  memcpy(k1, m->k_cache, cache * 2);
  memcpy(v1, m->v_cache, cache * 2);

  /* A whole batch, then the rest; then three uneven ones. */
  uint32_t splits[2][3] = { { LLM_BATCH, (uint32_t)n - LLM_BATCH, 0 },
                            { 5, LLM_BATCH, (uint32_t)n - 5 - LLM_BATCH } };

  for (int s = 0; s < 2; s++) {
    uint32_t done = 0;
    int ok = 1;

    llm_reset(m);
    t0 = now();
    for (int i = 0; i < 3 && splits[s][i] > 0; i++) {
      ok = ok && llm_step_batch(m, t + done, splits[s][i]) == 0;
      done += splits[s][i];
    }

    if (s == 0)
      printf("batch: %d tokens read in %.2f s a token at a time, %.2f s in batches of %d: "
             "%.1fx\n", n, one_at_a_time, now() - t0, LLM_BATCH, one_at_a_time / (now() - t0));

    int cached = 1;
    for (uint32_t l = 0; l < m->n_layer; l++) {
      size_t o = (size_t)l * m->n_ctx * m->kv_dim;
      cached = cached && memcmp(k1 + o, m->k_cache + o, used * 2) == 0
               && memcmp(v1 + o, m->v_cache + o, used * 2) == 0;
    }

    snprintf(what, sizeof what, "batch: read in batches of %u, %u and %u, the logits are "
             "the token-at-a-time ones to the bit", splits[s][0], splits[s][1], splits[s][2]);
    check(ok && m->n_past == (uint32_t)n && same_logits(m), what);
    snprintf(what, sizeof what, "batch: and so is every position of the cache (%u, %u, %u)",
             splits[s][0], splits[s][1], splits[s][2]);
    check(ok && cached, what);
  }

  /* Refused whole, with nothing read: more than a batch, and past the cache. */
  llm_reset(m);
  check(llm_step_batch(m, t, LLM_BATCH + 1) != 0 && m->n_past == 0,
        "batch: more than LLM_BATCH tokens refused, and nothing read");
  m->n_past = m->n_ctx - 1;
  check(llm_step_batch(m, t, 2) != 0 && m->n_past == m->n_ctx - 1,
        "batch: a batch past the cache's end refused, and nothing read");
  llm_reset(m);

  free(k1);
  free(v1);
}

/*
 * **A token's passage, recorded** (`llm_watch`; `docs/xavier.md`, *The LLM
 * Monitor: Inside*). A question longer than a batch read three ways: plain,
 * watched a token at a time, and watched in batches. Watching changes no
 * logit; the two watched readings record the same passage to the bit for
 * every token, since a batch is read as a token at a time is; at position
 * 0 every head looks at the token itself, wholly; and the last token's
 * last layer is held to what is left in the model's working vectors - its
 * residual stream's length summed here, and its neurons that fired
 * between the counts of its outputs above nought and at or above it. The
 * candidates: the greedy choice first, falling, the odds a softmax worked
 * out here.
 */
static struct llm_passage watched_one[2048], watched_batch[LLM_BATCH];

/* **The kit's trace** (`-DLLM_TRACE`), which the heads' check reads: each
 * layer's queries for the token being stepped, before RoPE - the rest of
 * the attention is worked out here from them and the cache. */
#define TRACE_LAYERS 64
static float *queries[TRACE_LAYERS];
static int capturing;
static float *gate_last;              /* the latest layer's gate, while capturing */
static uint32_t gate_room, gate_n;

void llm_trace(const char *name, uint32_t layer, const float *x, uint32_t n)
{
  /* And the last layer's gate, before SwiGLU: what Qwen2's neurons
   * firing is counted from. */
  if (capturing && !strcmp(name, "ffn_gate")) {
    if (!gate_last || n > gate_room) { free(gate_last); gate_last = malloc(n * sizeof *x); gate_room = n; }
    if (gate_last) memcpy(gate_last, x, n * sizeof *x);
    gate_n = n;
    return;
  }

  if (!capturing || layer >= TRACE_LAYERS || strcmp(name, "Qcur") != 0) return;

  if (!queries[layer]) queries[layer] = malloc(n * sizeof *x);
  if (queries[layer]) memcpy(queries[layer], x, n * sizeof *x);
}

/* RoPE as llama.cpp's NeoX pairing has it: element i with i + n_rot / 2,
 * at angles pos * base^(-2i / n_rot) - written here, not called, so that
 * the kit's own is not what it is held to. */
static void rope_here(float *v, uint32_t n_rot, uint32_t pos, float base)
{
  for (uint32_t i = 0; i < n_rot / 2; i++) {
    float theta = (float)pos;

    for (uint32_t k = 0; k < i; k++) theta *= (float)pow((double)base, -2.0 / (double)n_rot);

    float c = (float)cos((double)theta), s = (float)sin((double)theta);
    float a = v[i], b = v[i + n_rot / 2];

    v[i] = a * c - b * s;
    v[i + n_rot / 2] = a * s + b * c;
  }
}

/* Every head of every layer for the token at `pos`, from the queries
 * captured and the cache: how many of its looks agree with the record -
 * the same position, or one whose weight is within a rounding of the
 * strongest - and the share within a half-precision step. */
static int heads_agree(const struct llm_model *m, const struct llm_passage *p, uint32_t pos, int *looked)
{
  const uint32_t KV = m->kv_dim, D = m->head_dim;
  static double w[2048];
  float q[1024];
  int wrong = 0;

  *looked = 0;

  for (uint32_t l = 0; l < m->n_layer && l < LLM_PASSAGE_LAYERS && l < TRACE_LAYERS; l++) {
    const uint16_t *kc = m->k_cache + (size_t)l * m->n_ctx * KV;

    if (!queries[l]) return -1;

    for (uint32_t h = 0; h < m->n_head && h < LLM_PASSAGE_HEADS; h++) {
      double max = -1e300, sum = 0;
      uint32_t best = 0;
      /* The key head it reads: its own, or its group's (grouped-query
       * attention, as llama.cpp broadcasts K over the query heads). */
      const uint32_t kvh = h / (m->n_head / m->n_head_kv);

      memcpy(q, queries[l] + h * D, D * sizeof *q);
      rope_here(q, m->n_rot, pos, m->rope_base);

      for (uint32_t p0 = 0; p0 <= pos; p0++) {
        double d = 0;

        for (uint32_t i = 0; i < D; i++)
          d += (double)llm_f16_to_f32(llm_f32_to_f16(q[i] * (1.0f / sqrtf((float)D))))
               * llm_f16_to_f32(kc[(size_t)p0 * KV + kvh * D + i]);
        w[p0] = d;
        if (d > max) max = d;
      }

      for (uint32_t p0 = 0; p0 <= pos; p0++) sum += (w[p0] = exp(w[p0] - max));
      for (uint32_t p0 = 0; p0 <= pos; p0++) if (w[p0] > w[best]) best = p0;

      uint32_t theirs = pos - p->layer[l].back[h];
      double share = w[best] / sum, said = p->layer[l].share[h] / 65535.0;

      (*looked)++;
      if (p->layer[l].back[h] > pos || w[theirs] / sum < share - 2e-3 || fabs(said - share) > 2e-3) wrong++;
    }
  }

  return wrong;
}

static void check_passage(struct llm_model *m)
{
  static int32_t t[2048];
  static struct llm_passage last;
  static char text[4096];
  size_t at = 0;
  int n, same_records = 1, own = 1, bounds = 1;
  char what[200];

  at += (size_t)snprintf(text + at, sizeof text - at, "Instruct: Name the planets:");
  for (int i = 1; i <= 16; i++) at += (size_t)snprintf(text + at, sizeof text - at, " %d and", i);
  snprintf(text + at, sizeof text - at, " so on?\nOutput:");

  n = llm_tokenize(m, text, strlen(text), 1, t, 2048);
  if (n <= LLM_BATCH || (uint32_t)n > m->n_ctx || n > 2048) {
    check(0, "passage: the question is not more than one batch");
    return;
  }

  if (fresh_logits(m, t, n) != 0) {
    check(0, "passage: the question would not read");
    return;
  }

  /* Watched, a token at a time, the last one's queries captured. */
  llm_reset(m);
  for (int i = 0; i < n; i++) {
    llm_watch(m, &watched_one[i]);
    capturing = i == n - 1;
    if (llm_step(m, t[i]) != 0) { llm_watch(m, NULL); capturing = 0; check(0, "passage: a step refused"); return; }
  }
  llm_watch(m, NULL);
  capturing = 0;
  check(same_logits(m), "passage: watched a token at a time, the logits are the unwatched ones");

  {
    int looked, wrong = heads_agree(m, &watched_one[n - 1], (uint32_t)n - 1, &looked);

    snprintf(what, sizeof what, "passage: the last token's %d heads' strongest looks worked out here from its "
             "queries and the cache, %d not the record's", looked, wrong);
    check(wrong == 0 && looked > 0, what);
  }

  /* Watched, in batches: each batch's records against the single steps'. */
  llm_reset(m);
  for (int done = 0; done < n; ) {
    int k = n - done > (int)LLM_BATCH ? (int)LLM_BATCH : n - done;

    llm_watch(m, watched_batch);
    if (llm_step_batch(m, t + done, (uint32_t)k) != 0) {
      llm_watch(m, NULL);
      check(0, "passage: a batch refused");
      return;
    }
    llm_watch(m, NULL);

    for (int i = 0; i < k; i++) {
      if (memcmp(&watched_batch[i], &watched_one[done + i], sizeof watched_batch[i]) != 0) same_records = 0;
    }

    if (done + k == n) last = watched_batch[k - 1];
    done += k;
  }

  check(same_logits(m), "passage: watched in batches, the logits are the unwatched ones");
  snprintf(what, sizeof what, "passage: %d tokens read in batches recorded as a token at a time records them, "
           "to the bit", n);
  check(same_records, what);

  /* Position 0: one position to look at, wholly. */
  for (uint32_t l = 0; l < m->n_layer && l < LLM_PASSAGE_LAYERS; l++)
    for (uint32_t h = 0; h < m->n_head && h < LLM_PASSAGE_HEADS; h++)
      if (watched_one[0].layer[l].back[h] != 0 || watched_one[0].layer[l].share[h] != 65535) own = 0;
  check(own && watched_one[0].pos == 0 && watched_one[0].token == t[0] && watched_one[n - 1].pos == (uint32_t)n - 1
        && watched_one[0].n_layer == m->n_layer && watched_one[0].n_head == m->n_head,
        "passage: at position 0 every head looks at the token itself, wholly; positions and tokens as read");

  /* Every look within the text read, and every share a share. */
  for (int i = 0; i < n; i++)
    for (uint32_t l = 0; l < m->n_layer && l < LLM_PASSAGE_LAYERS; l++) {
      const struct llm_passage_layer *L = &watched_one[i].layer[l];

      if (L->fired > m->n_ff || !(L->stream > 0)) bounds = 0;
      for (uint32_t h = 0; h < m->n_head && h < LLM_PASSAGE_HEADS; h++)
        if (L->back[h] > (uint32_t)i || L->share[h] == 0) bounds = 0;
    }
  check(bounds, "passage: every head's look is to a token already read, with a share above nought; "
        "every layer's stream above nought and its neurons at most the layer's");

  /* The last token's last layer, against the working vectors it leaves. */
  {
    uint32_t L = m->n_layer - 1;
    const float *x = m->x + (size_t)(LLM_BATCH > 0 ? (uint32_t)((n - 1) % LLM_BATCH) : 0) * m->n_embd;
    /* What a neuron's firing is counted from: the MLP's input to GELU,
     * which phi-2 leaves in `ff`; Qwen2's gate, before SwiGLU made it
     * something else, as the trace caught it on the last token's last
     * layer. */
    const float *ff = m->kind == LLM_ARCH_QWEN2 ? gate_last
                      : m->ff + (size_t)((n - 1) % LLM_BATCH) * m->n_ff;
    double sq = 0;
    uint32_t above = 0, at_or_above = 0;

    for (uint32_t i = 0; i < m->n_embd; i++) sq += (double)x[i] * x[i];
    if (!ff || (m->kind == LLM_ARCH_QWEN2 && gate_n != m->n_ff)) ff = NULL;
    for (uint32_t i = 0; ff && i < m->n_ff; i++) { above += ff[i] > 0.0f; at_or_above += ff[i] >= 0.0f; }

    snprintf(what, sizeof what, "passage: the last layer's stream %.6g, its length worked out here %.6g; "
             "%u fired, between %u and %u", (double)last.layer[L].stream, sqrt(sq), last.layer[L].fired,
             above, at_or_above);
    check(ff && L < LLM_PASSAGE_LAYERS && last.layer[L].stream == (float)sqrt(sq) && last.layer[L].fired >= above
          && last.layer[L].fired <= at_or_above, what);
  }

  /* The candidates. */
  {
    double max = m->logits[0], sum = 0;
    int ok;

    llm_candidates(m, &last);
    for (uint32_t i = 1; i < m->n_vocab; i++) if (m->logits[i] > max) max = m->logits[i];
    for (uint32_t i = 0; i < m->n_vocab; i++) sum += exp((double)m->logits[i] - max);

    ok = last.n_cand == LLM_PASSAGE_CANDIDATES && last.cand[0] == llm_greedy(m);
    for (uint32_t k = 0; ok && k < last.n_cand; k++) {
      double want = exp((double)m->logits[last.cand[k]] - max) / sum;

      if (fabs((double)last.odds[k] - want) > 1e-6 * (want > 1e-3 ? want * 1e3 : 1.0)) ok = 0;
      if (k > 0 && (m->logits[last.cand[k]] > m->logits[last.cand[k - 1]] || last.cand[k] == last.cand[k - 1])) ok = 0;
    }

    /* And none left out larger than the last one kept. */
    for (uint32_t i = 0; ok && i < m->n_vocab; i++) {
      int kept = 0;

      for (uint32_t k = 0; k < last.n_cand; k++) kept |= last.cand[k] == (int32_t)i;
      if (!kept && m->logits[i] > m->logits[last.cand[last.n_cand - 1]]) ok = 0;
    }

    snprintf(what, sizeof what, "passage: ten candidates, the greedy choice first at %.3f, falling, the odds a "
             "softmax over all %u", (double)last.odds[0], m->n_vocab);
    check(ok, what);
  }
}

/*
 * **A pool of threads, as the server will have one a core** - here the
 * Mac's, through pthreads: `pool_run` hands each part to a worker and
 * waits for all of them.
 */
#define POOL 4

static struct {
  pthread_mutex_t lock;
  pthread_cond_t go, done;
  llm_part_fn *fn;
  void *job;
  uint32_t parts, gen, finished;
  pthread_t thread[POOL];
} pool = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER,
           NULL, NULL, 0, 0, 0, { 0 } };

static void *pool_worker(void *arg)
{
  uint32_t part = (uint32_t)(uintptr_t)arg, seen = 0;

  for (;;) {
    pthread_mutex_lock(&pool.lock);
    while (pool.gen == seen) pthread_cond_wait(&pool.go, &pool.lock);
    seen = pool.gen;
    pthread_mutex_unlock(&pool.lock);

    if (part < pool.parts) pool.fn(pool.job, part, pool.parts);

    pthread_mutex_lock(&pool.lock);
    if (++pool.finished == POOL) pthread_cond_signal(&pool.done);
    pthread_mutex_unlock(&pool.lock);
  }
  return NULL;
}

static void pool_run(void *ctx, llm_part_fn *fn, void *job, uint32_t parts)
{
  (void)ctx;
  pthread_mutex_lock(&pool.lock);
  pool.fn = fn;
  pool.job = job;
  pool.parts = parts;
  pool.finished = 0;
  pool.gen++;
  pthread_cond_broadcast(&pool.go);
  while (pool.finished < POOL) pthread_cond_wait(&pool.done, &pool.lock);
  pthread_mutex_unlock(&pool.lock);
}

/*
 * **The products cut across cores give the single thread's answer, to the
 * bit** (`docs/xavier.md`, *Making it fast*): a question read in batches
 * and an answer written a token at a time, each with one thread and with
 * four parts. The logits and every position of the cache must be equal.
 */
/*
 * **The stages tile the pass** (`llm_clock`): a clock that is only a count
 * of its own readings, so a batch's stages must add up to the readings
 * after its first - nothing counted twice, nothing between two stages lost
 * - and attention, the products and the norms each took some.
 */
static uint64_t readings;

static uint64_t count_readings(void)
{
  return readings++;
}

static void check_clock(struct llm_model *m)
{
  static int32_t t[2048];
  const char *q = "Instruct: What is a thread?\nOutput:";
  int n = llm_tokenize(m, q, strlen(q), 1, t, 2048);
  uint64_t before = 0, after = 0, r0;
  char what[200];

  if (n <= 0 || n > (int)LLM_BATCH) { check(0, "clock: the question would not read"); return; }

  llm_reset(m);
  memset(m->stage_counts, 0, sizeof m->stage_counts);
  llm_clock(m, count_readings);
  r0 = readings;
  if (llm_step_batch(m, t, (uint32_t)n) != 0) { llm_clock(m, NULL); check(0, "clock: a batch refused"); return; }
  for (uint32_t s = 0; s < LLM_STAGES_MAX; s++) after += m->stage_counts[s];
  llm_clock(m, NULL);

  /* And with no clock, nothing is counted. */
  for (uint32_t s = 0; s < LLM_STAGES_MAX; s++) before += m->stage_counts[s];
  llm_reset(m);
  if (llm_step_batch(m, t, (uint32_t)n) != 0) { check(0, "clock: a batch refused"); return; }
  for (uint32_t s = 0; s < LLM_STAGES_MAX; s++) before -= m->stage_counts[s];

  snprintf(what, sizeof what, "clock: %d tokens, the stages %llu of %llu readings after the first; "
           "attention %llu, products %llu, norms %llu; none without a clock", n,
           (unsigned long long)after, (unsigned long long)(readings - r0 - 1),
           (unsigned long long)m->stage_counts[LLM_STAGE_ATTENTION],
           (unsigned long long)m->stage_counts[LLM_STAGE_PRODUCTS],
           (unsigned long long)m->stage_counts[LLM_STAGE_NORM]);
  check(after == readings - r0 - 1 && m->stage_counts[LLM_STAGE_ATTENTION] > 0
        && m->stage_counts[LLM_STAGE_PRODUCTS] > 0 && m->stage_counts[LLM_STAGE_NORM] > 0
        && before == 0, what);
}

static void check_parallel(struct llm_model *m)
{
  static int32_t t[2048];
  static float one_logits[VOCAB_MAX];
  const char *q = "Instruct: Name three planets and say one thing about each.\nOutput:";
  int n = llm_tokenize(m, q, strlen(q), 1, t, 2048);
  const int steps = 12;
  double read[2], wrote[2];
  int32_t made[2][16] = { { 0 } };
  char what[200];

  if (n <= 0 || m->n_vocab > sizeof one_logits / sizeof *one_logits
      || (uint32_t)(n + steps) > m->n_ctx) {
    check(0, "parallel: the question would not fit");
    return;
  }

  for (uint32_t p = 0; p < POOL; p++)
    if (!pool.thread[p]) pthread_create(&pool.thread[p], NULL, pool_worker, (void *)(uintptr_t)p);

  size_t used = (size_t)(n + steps) * m->kv_dim, cache = (size_t)m->n_layer * m->n_ctx * m->kv_dim;
  uint16_t *k1 = malloc(cache * 2), *v1 = malloc(cache * 2);

  if (!k1 || !v1) { check(0, "parallel: no memory to compare the cache"); free(k1); free(v1); return; }

  for (int way = 0; way < 2; way++) {
    llm_parallel(m, way ? pool_run : NULL, NULL, way ? POOL : 1);
    llm_reset(m);

    double t0 = now();
    for (int done = 0; done < n; ) {
      int k = n - done > LLM_BATCH ? LLM_BATCH : n - done;
      if (llm_step_batch(m, t + done, (uint32_t)k) != 0) { check(0, "parallel: a batch refused"); goto out; }
      done += k;
    }
    read[way] = now() - t0;

    t0 = now();
    for (int i = 0; i < steps; i++) {
      made[way][i] = llm_greedy(m);
      /* Poisoned, so a row no part wrote cannot pass for one that was. */
      memset(m->logits, 0xff, m->n_vocab * sizeof *m->logits);
      if (llm_step(m, made[way][i]) != 0) { check(0, "parallel: a step refused"); goto out; }
    }
    wrote[way] = now() - t0;

    if (way == 0) {
      memcpy(one_logits, m->logits, m->n_vocab * sizeof *one_logits);
      memcpy(k1, m->k_cache, cache * 2);
      memcpy(v1, m->v_cache, cache * 2);
    }
  }

  int cached = 1;
  for (uint32_t l = 0; l < m->n_layer; l++) {
    size_t o = (size_t)l * m->n_ctx * m->kv_dim;
    cached = cached && memcmp(k1 + o, m->k_cache + o, used * 2) == 0
             && memcmp(v1 + o, m->v_cache + o, used * 2) == 0;
  }

  snprintf(what, sizeof what, "parallel: %d parts, the same %d tokens made and the last logits "
           "the single thread's to the bit", POOL, steps);
  check(memcmp(made[0], made[1], (size_t)steps * sizeof made[0][0]) == 0
        && memcmp(one_logits, m->logits, m->n_vocab * sizeof *one_logits) == 0, what);
  check(cached, "parallel: and every position of the cache");
  if (getenv("LLM_DEBUG")) {
    int d = 0;
    for (uint32_t i = 0; i < m->n_vocab && d < 5; i++)
      if (memcmp(&one_logits[i], &m->logits[i], 4) != 0) {
        printf("  logit %u: %.9g against %.9g\n", i, one_logits[i], m->logits[i]);
        d++;
      }
  }

  printf("parallel: %d parts on the Mac: a %d-token question %.2f s -> %.2f s (%.1fx), "
         "%d tokens written %.2f s -> %.2f s (%.1fx; %.1f -> %.1f tokens a second)\n",
         POOL, n, read[0], read[1], read[0] / read[1], steps, wrote[0], wrote[1],
         wrote[0] / wrote[1], steps / wrote[0], steps / wrote[1]);

out:
  llm_parallel(m, NULL, NULL, 1);
  llm_reset(m);
  free(k1);
  free(v1);
}

int main(int argc, char **argv)
{
  if (argc > 1 && !strcmp(argv[1], "--gpu")) {
    with_gpu = 1;
    argv++;
    argc--;
  }

  if (argc < 3) {
    fprintf(stderr, "usage: test_llm [--gpu] MODEL.gguf REFERENCE.txt\n");
    return 2;
  }

  int fd = open(argv[1], O_RDONLY);
  struct stat st;

  if (fd < 0 || fstat(fd, &st) < 0) {
    printf("SKIP: no %s - docs/xavier.md X1 says how it is made\n", argv[1]);
    return 0;
  }

  void *file = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  FILE *ref = fopen(argv[2], "r");

  if (file == MAP_FAILED || !ref) {
    printf("FAIL: could not read %s or %s\n", argv[1], argv[2]);
    return 1;
  }

  static struct llm_model m;
  char why[160];
  double t0 = now();

  if (llm_open(&m, file, (size_t)st.st_size, 256, alloc, NULL, why, sizeof why) < 0) {
    printf("FAIL: the kit would not open %s: %s\n", argv[1], why);
    return 1;
  }

  printf("opened %.*s: %u layers, %u wide, %u heads, %u tokens, %.2f s\n", (int)m.name.n,
         m.name.s, m.n_layer, m.n_embd, m.n_head, m.n_vocab, now() - t0);

  check_gelu();

  if (with_gpu) {
    static const struct llm_gpu_device dev = { NULL, emu_kernel_given, emu_buffer, emu_forget, emu_run, emu_wait };

    if (llm_gpu_begin(&gpu, &dev, why, sizeof why) != 0) {
      printf("FAIL: the GPU path would not begin: %s\n", why);
      return 1;
    }

    llm_offload(&m, llm_gpu_product, llm_gpu_done, &gpu);
  }

  static char line[1 << 16];
  static int32_t got[4096];
  static int want[4096];
  static char text[8192];
  int prompts = 0, steps = 0, prompt_n = 0, answer = 0, ties = 0;
  double spent = 0;

  while (fgets(line, sizeof line, ref)) {
    char *colon = strstr(line, " :");

    if (!strncmp(line, "tokens ", 7) && colon) {
      int special = line[7] == '1';
      size_t n = unhex(line + 9, text, sizeof text);
      int nw = numbers(colon + 2, want, 4096);
      int ng = llm_tokenize(&m, text, n, special, got, 4096);
      int same = ng == nw;

      for (int i = 0; same && i < nw; i++) same = got[i] == want[i];

      char what[160];
      snprintf(what, sizeof what, "tokens of \"%.40s\"%s: %d against llama.cpp's %d", text,
               special ? " (special)" : "", ng, nw);
      check(same, what);

      if (!same) {
        printf("    kit:");
        for (int i = 0; i < ng && i < 40; i++) printf(" %d", got[i]);
        printf("\n    ref:");
        for (int i = 0; i < nw && i < 40; i++) printf(" %d", want[i]);
        printf("\n");
      }
    } else if (!strncmp(line, "prompt ", 7) && colon) {
      int nw = numbers(colon + 2, want, 4096);

      prompts++;
      prompt_n = nw;
      answer = 0;
      llm_reset(&m);
      t0 = now();
      for (int i = 0; i < nw; i++) llm_step(&m, want[i]);
      spent += now() - t0;
      steps += nw;
    } else if (!strncmp(line, "top ", 4)) {
      int ids[10];
      float logits[10];
      int n = 0;
      char *p = line + 4;

      while (n < 10 && sscanf(p, "%d:%f", &ids[n], &logits[n]) == 2) {
        n++;
        p = strchr(p, ' ');
        if (!p) break;
        p++;
      }

      float worst = 0;
      int same = 1;

      for (int i = 0; i < n; i++) {
        float d = m.logits[ids[i]] - logits[i];
        if (d < 0) d = -d;
        if (d > worst) worst = d;
      }
      /* Llama.cpp's top token, or a near-tie: its choice and the kit's
       * within NEAR of each other on both sides. */
      int32_t mine = llm_greedy(&m);
      float theirs_of_mine = -1e30f;
      for (int i = 0; i < n; i++) if (ids[i] == mine) theirs_of_mine = logits[i];
      same = mine == ids[0] || (theirs_of_mine >= logits[0] - NEAR
                                && m.logits[ids[0]] >= m.logits[mine] - NEAR);
      float allowed = prompt_n == 1 ? EXACT : TOLERANCE;

      char what[160];
      snprintf(what, sizeof what, "prompt %d's top token %d against llama.cpp's %d, not a "
               "near-tie", prompts, mine, ids[0]);
      check(same, what);
      snprintf(what, sizeof what, "prompt %d (%d tokens): the ten largest logits within %.2f: "
               "worst %.4f", prompts, prompt_n, allowed, worst);
      check(worst <= allowed, what);
      printf("prompt %d, %d tokens: the ten largest logits within %.4f of llama.cpp's\n",
             prompts, prompt_n, worst);
    } else if (!strncmp(line, "step ", 5)) {
      int ids[3];
      float logits[3];
      int n = 0;
      char *p = strchr(line + 5, ' ');

      while (p && n < 3 && sscanf(p + 1, "%d:%f", &ids[n], &logits[n]) == 2) {
        n++;
        p = strchr(p + 1, ' ');
      }

      int32_t mine = llm_greedy(&m);
      int agree = mine == ids[0];

      if (!agree) {
        /* A near-tie: llama.cpp rates the kit's choice within NEAR of its
         * best, and the kit rates llama.cpp's within NEAR of its own. */
        float theirs_of_mine = -1e30f;
        for (int i = 0; i < n; i++) if (ids[i] == mine) theirs_of_mine = logits[i];
        int tie = theirs_of_mine >= logits[0] - NEAR
                  && m.logits[ids[0]] >= m.logits[mine] - NEAR;
        char what[160];
        snprintf(what, sizeof what, "prompt %d, answer step %d: %d against llama.cpp's %d, "
                 "not a near-tie", prompts, answer, mine, ids[0]);
        check(tie, what);
        ties++;
      } else {
        checks++;
      }

      char said[64];
      int sn = llm_token_text(&m, ids[0], said, (int)sizeof said - 1);
      said[sn] = 0;
      if (answer == 0) printf("prompt %d answers:", prompts);
      printf("%s", said);

      t0 = now();
      llm_step(&m, ids[0]);                 /* llama.cpp's token, always */
      spent += now() - t0;
      steps++;
      answer++;
    }
  }

  check_held(&m);
  check_batch(&m);
  check_parallel(&m);
  check_passage(&m);
  check_clock(&m);
  if (with_gpu) check_gpu(&m);

  printf("\n%d steps a near-tie chosen the other way\n", ties);
  printf("%d steps in %.1f s: %.2f tokens a second, scalar, one thread, on the Mac\n", steps,
         spent, steps / spent);

  if (failed) {
    printf("FAIL: %d of %d checks on the LLM Kit against llama.cpp\n", failed, checks);
    return 1;
  }

  printf("PASS: %d checks on the LLM Kit against llama.cpp%s (%.*s: every text's tokens, plain and "
         "special; one token's logits within %.2f; each prompt's top token and ten largest logits "
         "within %.2f; every answer step llama.cpp's choice or a near-tie within %.2f, %d of "
         "them)\n", checks, with_gpu ? ", its products on the GPU in the emulator" : "",
         (int)m.name.n, m.name.s, EXACT, TOLERANCE, NEAR, ties);
  return 0;
}
