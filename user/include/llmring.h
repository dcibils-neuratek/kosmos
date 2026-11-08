/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_LLMRING_H
#define KOSMOS_LLMRING_H

/*
 * **An answer, as the LLM server writes it** (`llmproto.h`'s ASK): a region
 * the server makes and hands back, in which the answer's bytes accumulate
 * from `LLM_ANSWER_DATA` on as the tokens are made. **Append only**, and
 * that is what makes it simple: one side writes, the other reads, and the
 * only number both touch is `written`, which only grows. So there is no
 * read index to hand back and nothing to wrap - an answer is bounded by its
 * `max_tokens`, and the region is sized for the most a request may ask.
 *
 * The server writes the bytes, then publishes `written` after a barrier;
 * a reader loads `written` with acquire (`sys.region_load32`, or
 * `llm_answer_acquire` in C) and may then read every byte below it. `ended`
 * is published last of all, once nothing more will come.
 */

#include <stdint.h>

#if defined(__aarch64__)
#define LLM_ANSWER_BARRIER() __asm__ volatile("dmb ish" ::: "memory")
#else
#define LLM_ANSWER_BARRIER() __asm__ volatile("" ::: "memory")   /* x86 orders stores */
#endif

#define LLM_ANSWER_MAGIC 0x4c4c4d41u      /* "AMLL" */
#define LLM_ANSWER_DATA  4096u
#define LLM_ANSWER_BYTES (60u * 1024u)    /* an answer's room */
#define LLM_ANSWER_REGION (LLM_ANSWER_DATA + LLM_ANSWER_BYTES)
#define LLM_ANSWER_TOKENS_MAX 2048u       /* a request may not ask more */

struct llm_answer {
    uint32_t magic;
    uint32_t bytes;                       /* room from LLM_ANSWER_DATA */
    volatile uint32_t written;
    volatile uint32_t ended;
    volatile uint32_t tokens;             /* answer tokens so far */
    uint32_t reserved[3];
};

_Static_assert(sizeof(struct llm_answer) == 32, "llm_answer is 32 bytes");
_Static_assert(__builtin_offsetof(struct llm_answer, written) == 8, "written at 8");
_Static_assert(__builtin_offsetof(struct llm_answer, ended) == 12, "ended at 12");
_Static_assert(__builtin_offsetof(struct llm_answer, tokens) == 16, "tokens at 16");

static inline void llm_answer_publish(volatile uint32_t *at, uint32_t value)
{
    LLM_ANSWER_BARRIER();
    *at = value;
}

static inline uint32_t llm_answer_acquire(const volatile uint32_t *at)
{
    uint32_t v = *at;
    LLM_ANSWER_BARRIER();
    return v;
}

#endif
