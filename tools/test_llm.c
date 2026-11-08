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

#include <fcntl.h>
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

static int checks, failed;

static void check(int ok, const char *what)
{
  checks++;
  if (!ok) { failed++; printf("  FAIL: %s\n", what); }
}

static void *alloc(void *ctx, size_t bytes)
{
  (void)ctx;
  return calloc(1, bytes ? bytes : 1);
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

int main(int argc, char **argv)
{
  if (argc < 3) {
    fprintf(stderr, "usage: test_llm MODEL.gguf REFERENCE.txt\n");
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

  printf("\n%d steps a near-tie chosen the other way\n", ties);
  printf("%d steps in %.1f s: %.2f tokens a second, scalar, one thread, on the Mac\n", steps,
         spent, steps / spent);

  if (failed) {
    printf("FAIL: %d of %d checks on the LLM Kit against llama.cpp\n", failed, checks);
    return 1;
  }

  printf("PASS: %d checks on the LLM Kit against llama.cpp (%.*s: every text's tokens, plain and "
         "special; one token's logits within %.2f; each prompt's top token and ten largest logits "
         "within %.2f; every answer step llama.cpp's choice or a near-tie within %.2f, %d of "
         "them)\n", checks, (int)m.name.n, m.name.s, EXACT, TOLERANCE, NEAR, ties);
  return 0;
}
