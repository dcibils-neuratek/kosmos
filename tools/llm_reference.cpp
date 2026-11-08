//  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
//
// **llama.cpp's answers, written down** for `tools/test_llm.c` to hold the
// LLM Kit to (`docs/xavier.md` X1). Built on the Mac against llama.cpp
// itself (`make llm-reference`), run once per model, and its output kept
// in the tree as `tools/llm_reference_<model>.txt` - so the host test needs
// the model file and our C, and not llama.cpp.
//
// For each text in CASES: its tokens, as plain text and with special
// tokens parsed. For each prompt in PROMPTS: fed a token at a time, as the
// kit feeds it, the ten largest logits after it; then the greedy answer
// for STEPS tokens, each step with its three largest logits - so the test
// can feed the kit llama.cpp's own answer and ask, at every step, whether
// the two agree on what comes next.
//
// **Why not the same answer token for token**: llama.cpp does not give
// that against itself. Built plain (`GGML_NATIVE=OFF`, no repacking) and
// built for this Mac's NEON, it differs by up to 0.62 on a logit after a
// dozen tokens and chooses differently at near-ties - the order a sum is
// taken in moves a Q8_0 rounding, and that grows (`docs/xavier.md`, X1).
//
// Flash attention off and one sequence, so the arithmetic is the plain
// path the kit follows.
//
// Usage: llm_reference MODEL.gguf > tools/llm_reference_<model>.txt

#include "llama.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const char *CASES[] = {
    "Hello, world!",
    "I'm sure it's fine; they're here, we've won, you'll see, he'd go.",
    "In 2026 there were 3.14159 apples and 42 pears.",
    "a  b   c\n\n\tindented    four spaces\n",
    "!!! ??? ... --- (ok) [x] {y} <z> @#$%^&*",
    "Caf\xc3\xa9 na\xc3\xafve \xc3\xbc" "ber \xc3\x97 \xc3\xb7 \xc2\xb5m \xc2\xbd",
    "<|endoftext|>Next document.",
    "def f(x):\n    return x ** 2\n\n\nprint(f(3))",
    "               many spaces, then a word",
    "",
};

static const char *PROMPTS[] = {
    "def",                     // one token: the arithmetic alone, held tight
    "Instruct: What is the capital of France?\nOutput:",
    "def fibonacci(n):",
};

static const int STEPS = 12;

static std::vector<llama_token> tokenize(const llama_vocab *vocab, const std::string &text,
                                         bool special)
{
    std::vector<llama_token> out(text.size() + 16);
    int n = llama_tokenize(vocab, text.data(), (int32_t)text.size(), out.data(),
                           (int32_t)out.size(), false, special);
    out.resize(n < 0 ? 0 : n);
    return out;
}

static void hex(const char *s)
{
    for (const char *p = s; *p; p++) printf("%02x", (unsigned char)*p);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: llm_reference MODEL.gguf\n");
        return 2;
    }

    llama_backend_init();
    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    llama_model *model = llama_model_load_from_file(argv[1], mp);
    if (!model) return 1;

    const llama_vocab *vocab = llama_model_get_vocab(model);
    const int n_vocab = llama_vocab_n_tokens(vocab);

    printf("# llama.cpp's tokens and answers for %s\n", argv[1]);

    for (const char *text : CASES) {
        for (int special = 0; special < 2; special++) {
            printf("tokens %d ", special);
            hex(text);
            printf(" :");
            for (llama_token t : tokenize(vocab, text, special)) printf(" %d", t);
            printf("\n");
        }
    }

    for (const char *prompt : PROMPTS) {
        llama_context_params cp = llama_context_default_params();
        cp.n_ctx = 256;
        cp.n_batch = 1;
        cp.n_ubatch = 1;
        cp.n_threads = 1;
        cp.n_threads_batch = 1;
        cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
        llama_context *ctx = llama_init_from_model(model, cp);
        if (!ctx) return 1;

        std::vector<llama_token> toks = tokenize(vocab, prompt, false);
        printf("prompt ");
        hex(prompt);
        printf(" :");
        for (llama_token t : toks) printf(" %d", t);
        printf("\n");

        for (size_t i = 0; i < toks.size(); i++) {
            llama_batch b = llama_batch_get_one(&toks[i], 1);
            if (llama_decode(ctx, b)) return 1;
        }

        // The ten largest logits after the prompt.
        const float *logits = llama_get_logits_ith(ctx, -1);
        std::vector<int> order(n_vocab);
        for (int i = 0; i < n_vocab; i++) order[i] = i;
        std::partial_sort(order.begin(), order.begin() + 10, order.end(),
                          [&](int a, int b) { return logits[a] > logits[b]; });
        printf("top");
        for (int i = 0; i < 10; i++) printf(" %d:%.4f", order[i], logits[order[i]]);
        printf("\n");

        // And the greedy answer, a step a line, with its three best.
        for (int s = 0; s < STEPS; s++) {
            const float *l = llama_get_logits_ith(ctx, -1);
            std::partial_sort(order.begin(), order.begin() + 3, order.end(),
                              [&](int a, int b) { return l[a] > l[b]; });
            llama_token best = order[0];
            printf("step %d", best);
            for (int i = 0; i < 3; i++) printf(" %d:%.4f", order[i], l[order[i]]);
            printf("\n");
            llama_batch b = llama_batch_get_one(&best, 1);
            if (llama_decode(ctx, b)) return 1;
        }

        llama_free(ctx);
    }

    llama_model_free(model);
    return 0;
}
