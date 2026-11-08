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

int main(int argc, char **argv)
{
  struct llm_model m;
  struct stat st;
  char why[128];
  int fd;

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
