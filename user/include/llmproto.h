/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_LLMPROTO_H
#define KOSMOS_LLMPROTO_H

/*
 * **The LLM server's protocol** (`docs/xavier.md` X3): `/Running/llm`, the
 * one process that holds a language model for every application that asks.
 * A declared shape, as every system server speaks, so a caller can say only
 * what there are fields for.
 *
 *   LOAD   a model, by its file's name in `/Home/Models` - the only folder
 *          the server can read. The reply is held until the model is in
 *          memory, or refused; the server answers everyone else meanwhile.
 *   ASK    a prompt, as text, and the most tokens to answer with. The reply
 *          carries the answer's region (`llmring.h`) as its capability, and
 *          the server writes the answer into it as it is made.
 *   WAIT   "tell me when the answer has gone past `since` bytes, or ended":
 *          held until it has - so nobody polls, and nothing sleeps.
 *   STOP   ends the answer being made, where it is.
 *   STATS  what is loaded and how fast it ran: the engine's numbers, which
 *          `llm` at the prompt and the monitor window read (X8b).
 *
 * **One answer at a time**: there is one model and one cache. A second ASK
 * while one is being answered is refused as BUSY, and says so; a queue is
 * X8b's, with the monitor that would show it.
 *
 * **Times are microseconds**, converted once, here, from the counter: a
 * number that crosses a boundary says its unit in its name (`CLAUDE.md`,
 * *Two clocks*).
 */

#include <stdint.h>

#define LLM_OP_LOAD   1u
#define LLM_OP_ASK    2u
#define LLM_OP_WAIT   3u
#define LLM_OP_STOP   4u
#define LLM_OP_STATS  5u

#define LLM_OK            0u
#define LLM_ERR_BAD_OP    1u    /* not a request this protocol has */
#define LLM_ERR_NO_FILE   2u    /* no such model in /Home/Models */
#define LLM_ERR_BAD_MODEL 3u    /* the file is not a model this kit runs */
#define LLM_ERR_NO_MEMORY 4u
#define LLM_ERR_NO_MODEL  5u    /* ASK with nothing loaded */
#define LLM_ERR_BUSY      6u    /* a LOAD or an answer is under way */
#define LLM_ERR_TOO_LONG  7u    /* the prompt is more than the cache holds */
#define LLM_ERR_NO_ANSWER 8u    /* WAIT or STOP with no answer to wait on */

#define LLM_NAME_MAX  64u
#define LLM_TEXT_MAX  1536u     /* a prompt, for now: one message of text */
#define LLM_WHY_MAX   96u

/* What a request may ask for besides its tokens. */
#define LLM_STOP_AT_BLANK_LINE 1u   /* end the answer at its first "\n\n" */

struct llm_request {
    uint32_t op;
    uint32_t max_tokens;            /* ASK: at most this many; 0 is 128   */
    uint32_t flags;                 /* ASK: LLM_STOP_*                     */
    uint32_t since;                 /* WAIT: the bytes already seen        */
    uint32_t length;                /* ASK: bytes of `text`                */
    uint32_t reserved;
    char     name[LLM_NAME_MAX];    /* LOAD: a file in /Home/Models        */
    char     text[LLM_TEXT_MAX];    /* ASK: the prompt, not terminated     */
};

/* The server's state, in every reply. */
#define LLM_STATE_EMPTY     0u      /* nothing loaded */
#define LLM_STATE_LOADING   1u
#define LLM_STATE_READY     2u
#define LLM_STATE_ANSWERING 3u

struct llm_reply {
    uint32_t error;
    uint32_t state;
    uint32_t written;               /* WAIT, STOP: the answer's bytes      */
    uint32_t ended;                 /* WAIT, STOP: 1 when no more will come */

    /* STATS, and LOAD's answer: the model as it is held. */
    char     model[LLM_NAME_MAX];   /* the file's name                     */
    char     title[LLM_NAME_MAX];   /* what the file calls itself          */
    char     arch[16];              /* general.architecture: "phi2"        */
    char     why[LLM_WHY_MAX];      /* a refused LOAD, said by the kit     */
    uint64_t model_bytes;
    uint64_t loaded_bytes;          /* while loading: how far              */
    uint32_t n_layer, n_embd, n_vocab, n_ctx;

    /* The last answer, as it went, or as it is going. */
    uint32_t prompt_tokens;
    uint32_t answer_tokens;
    uint64_t prompt_us;             /* reading the prompt                  */
    uint64_t answer_us;             /* writing the answer                  */
    uint64_t first_us;              /* the ask to the first answer token   */
    uint64_t load_us;               /* the last LOAD, the file to ready    */
    uint64_t answers;               /* since the server started            */
};

_Static_assert(sizeof(struct llm_request) <= 2048, "an LLM request must fit in one message");
_Static_assert(sizeof(struct llm_reply) <= 2048, "an LLM reply must fit in one message");

#endif
