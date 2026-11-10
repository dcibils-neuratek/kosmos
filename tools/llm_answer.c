/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The LLM Kit's own answer to a prompt, on the Mac** (`docs/xavier.md`
 * X3): what `tools/run_llm.py` holds the LLM server to. The kit is held to
 * llama.cpp by `test_llm`; this is the same kit, and the guest's answer has
 * to be its answer, byte for byte - which is what says the server, its
 * protocol, its region and the kit compiled freestanding for Kosmos change
 * nothing.
 *
 *   llm_answer MODEL TOKENS HEX...
 *
 * prints, for each prompt (its bytes in hex), the greedy answer as the
 * server makes it - the prompt read with special tokens, then up to TOKENS
 * tokens, ending early at the model's end of text - each token's bytes in
 * hex and a dot between tokens, so a caller can say where an answer asked
 * to stop at a blank line has to end:
 *
 *   answer HEX : TOKEN-HEX.TOKEN-HEX...
 *
 * **With `--chain`, a conversation** (X4): the prompts are the texts a
 * window asked one after another, each the whole conversation so far, and
 * the cache is held across them through the kit's own `llm_prompt` and
 * `llm_feed`, as the server holds it - so a follow-up keeps what the
 * server would keep. `--stops HEX,HEX` are the texts an answer ends at,
 * cut as the server cuts them. Each answer is then its bytes, whole:
 *
 *   llm_answer --chain [--stops HEX,HEX] MODEL TOKENS [SEED/]HEX...
 *   answer HEX : ANSWER-HEX kept K read R held H
 */

#include "llm_core.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void *alloc(void *ctx, size_t bytes)
{
  (void)ctx;
  return calloc(1, bytes ? bytes : 1);
}

static int unhex(const char *h, char *out, int room)
{
  int n = 0;

  for (; h[0] && h[1] && n < room; h += 2) {
    unsigned v;
    if (sscanf(h, "%2x", &v) != 1) return -1;
    out[n++] = (char)v;
  }

  return n;
}

/* The server's `choose`. */
static int32_t pick(struct llm_model *m, const struct llm_held *h, uint32_t seed, uint64_t *rng)
{
  if (seed == 0) return llm_greedy(m);

  return llm_sample(m, h->tokens, h->n, &llm_sampling_default, rng);
}

/* The prompts as a conversation: one held context across all of them. */
static int chain(struct llm_model *m, int tokens, char stops[2][64], int stop_n[2], int count,
                 char **prompts)
{
  static int32_t held_tokens[65536], room[65536];
  static char held_text[262144], text[262144], made[65536];
  struct llm_held h = { held_tokens, 0, held_text, sizeof held_text, 0 };

  llm_held_reset(m, &h);

  for (int a = 0; a < count; a++) {
    /* `SEED/HEX`: drawn from that seed, as the server draws an answer asked
     * with it; plain `HEX`, the likeliest each time. */
    const char *slash = strchr(prompts[a], '/');
    uint32_t seed = slash ? (uint32_t)strtoul(prompts[a], NULL, 10) : 0;
    uint64_t rng = llm_seed(seed);
    int n = unhex(slash ? slash + 1 : prompts[a], text, (int)sizeof text);
    uint32_t kept = 0;
    int k = n > 0 ? llm_prompt(m, &h, text, (size_t)n, room, (int)m->n_ctx + 1, &kept) : -1;
    int bytes = 0;

    printf("answer %s :", prompts[a]);

    if (k <= 0) {
      printf(" refused\n");
      continue;
    }

    for (int i = 0; i < k; i++) llm_feed(m, &h, room[i]);

    int32_t next = pick(m, &h, seed, &rng);

    /* As the server's `ask_step`: a token written, then the stops looked
     * for, then - unless it was the last - fed. */
    for (int done = 0, count_made = 0; !done;) {
      char piece[256];

      if (next == m->eos || m->n_past >= m->n_ctx) break;

      int b = llm_token_text(m, next, piece, (int)sizeof piece);

      if (b < 0 || bytes + b > (int)sizeof made) break;

      memcpy(made + bytes, piece, (size_t)b);
      bytes += b;
      count_made++;

      for (int s = 0; s < 2 && !done; s++) {
        if (stop_n[s] == 0) continue;

        for (int i = 0; i + stop_n[s] <= bytes; i++) {
          if (memcmp(made + i, stops[s], (size_t)stop_n[s]) == 0) {
            bytes = i;
            done = 1;
            break;
          }
        }
      }

      if (done || count_made >= tokens || llm_feed(m, &h, next) != 0) break;

      next = pick(m, &h, seed, &rng);
    }

    printf(" ");
    for (int i = 0; i < bytes; i++) printf("%02x", (unsigned char)made[i]);
    printf(" kept %u read %d held %u\n", kept, k, m->n_past);
  }

  return 0;
}

int main(int argc, char **argv)
{
  struct llm_model m;
  struct stat st;
  char why[128];
  int fd;
  int chained = 0;
  static char stops[2][64];
  int stop_n[2] = { 0, 0 };

  while (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-') {
    if (strcmp(argv[1], "--chain") == 0) {
      chained = 1;
      argv++, argc--;
    } else if (strcmp(argv[1], "--stops") == 0 && argc > 2) {
      char *at = argv[2];

      for (int s = 0; s < 2 && at && *at; s++) {
        char *comma = strchr(at, ',');

        if (comma) *comma = '\0';
        stop_n[s] = unhex(at, stops[s], (int)sizeof stops[s]);
        at = comma ? comma + 1 : NULL;
      }

      argv += 2, argc -= 2;
    } else {
      break;
    }
  }

  if (argc < 4) {
    fprintf(stderr, "usage: llm_answer MODEL TOKENS HEX...\n");
    return 2;
  }

  if ((fd = open(argv[1], O_RDONLY)) < 0 || fstat(fd, &st) != 0) {
    fprintf(stderr, "llm_answer: cannot read %s\n", argv[1]);
    return 1;
  }

  void *file = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

  memset(&m, 0, sizeof m);

  /* n_ctx 0, as the server asks: everything the model was trained to hold. */
  if (file == MAP_FAILED || llm_open(&m, file, (size_t)st.st_size, 0, alloc, NULL, why, sizeof why) < 0) {
    fprintf(stderr, "llm_answer: %s\n", why);
    return 1;
  }

  int tokens = atoi(argv[2]);

  if (chained) return chain(&m, tokens, stops, stop_n, argc - 3, argv + 3);

  for (int a = 3; a < argc; a++) {
    static char text[4096];
    static int32_t ids[4097];
    int n = unhex(argv[a], text, (int)sizeof text);
    int k = n >= 0 ? llm_tokenize(&m, text, (size_t)n, 1, ids, 4097) : -1;

    printf("answer %s :", argv[a]);

    if (k <= 0) {
      printf(" refused\n");
      continue;
    }

    llm_reset(&m);

    for (int i = 0; i < k; i++) llm_step(&m, ids[i]);

    int32_t next = llm_greedy(&m);
    printf(" ");

    for (int made = 0; made < tokens; made++) {
      char piece[256];

      if (next == m.eos || m.n_past >= m.n_ctx) break;

      int b = llm_token_text(&m, next, piece, (int)sizeof piece);

      if (made > 0) printf(".");

      for (int i = 0; i < b; i++) printf("%02x", (unsigned char)piece[i]);

      if (made + 1 >= tokens || llm_step(&m, next) != 0) break;

      next = llm_greedy(&m);
    }

    printf("\n");
  }

  return 0;
}
