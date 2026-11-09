/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_LLMPROTO_H
#define KOSMOS_LLMPROTO_H

/*
 * **The LLM server's protocol** (`docs/xavier.md` X3): `/Running/llm`, the
 * one process that holds a language model for every application that asks.
 * A declared shape, as every system server speaks, so a caller can say only
 * what there are fields for.
 *
 *   LOAD   a model, by its file's name in `/Models` (`/Home/Models` without
 *          that drive) - the only folder
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
 *   SETTINGS  where the model runs - how many cores, which of the kit's
 *          loops, the GPU - and what this machine has to choose from; with
 *          LLM_SETTINGS_CHANGE, changed first. **The server's, for every
 *          application that asks** (Diego, 8 November 2026: "server
 *          settings for everyone"), kept beside the models across restarts,
 *          and in force from the next question (`xavier.md`, *Settings*).
 *
 * **A conversation is asked whole, and only its new words are read**
 * (X4). An application sends everything said so far with each turn - in a
 * region handed with the request when it is more than a message holds
 * (`LLM_TEXT_IN_REGION`) - and the server keeps what its cache already
 * holds of it (`llm_core.h`, `struct llm_held`): a follow-up reads its own
 * words, and the reply says how many were kept.
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
#define LLM_OP_SETTINGS 6u

#define LLM_OK            0u
#define LLM_ERR_BAD_OP    1u    /* not a request this protocol has */
#define LLM_ERR_NO_FILE   2u    /* no such model in the models folder */
#define LLM_ERR_BAD_MODEL 3u    /* the file is not a model this kit runs */
#define LLM_ERR_NO_MEMORY 4u
#define LLM_ERR_NO_MODEL  5u    /* ASK with nothing loaded */
#define LLM_ERR_BUSY      6u    /* a LOAD or an answer is under way */
#define LLM_ERR_TOO_LONG  7u    /* the prompt is more than the cache holds */
#define LLM_ERR_NO_ANSWER 8u    /* WAIT or STOP with no answer to wait on */
#define LLM_ERR_CANNOT    9u    /* SETTINGS: more cores than there are, or
                                   loops or a GPU this machine has not got */
#define LLM_ERR_NOT_KEPT 10u    /* SETTINGS: in force, but the file beside
                                   the models could not be written */

#define LLM_NAME_MAX  64u
#define LLM_TEXT_MAX  1536u     /* a prompt in the message itself */
#define LLM_WHY_MAX   96u
#define LLM_STOP_MAX  24u       /* a stop text, unterminated when it fills */
#define LLM_STOPS     2u

/* A conversation in a region: at most this many bytes, from its start. */
#define LLM_CONVERSATION_MAX (256u * 1024u)

/* What a request may ask for besides its tokens. */
#define LLM_STOP_AT_BLANK_LINE 1u   /* ASK: end the answer at its first "\n\n"  */
#define LLM_TEXT_IN_REGION     2u   /* ASK: `length` bytes are in the region
                                       handed with the request, not `text` */
#define LLM_LOAD_NOWAIT        4u   /* LOAD: answer at once; STATS says when
                                       the model is in, so a window need not
                                       wait on it */
#define LLM_SETTINGS_CHANGE    8u   /* SETTINGS: to `settings`, then answer */

/*
 * **Where the model runs.** Every choice of loops gives the same answer to
 * the bit (`llm_dot.h`); only the speed differs. `cores` 0 is Auto: one a
 * core, not one a processor, since a core's second processor waits on the
 * same memory. `loops` AUTO is the fastest this processor has.
 */
#define LLM_LOOPS_AUTO   0u
#define LLM_LOOPS_PLAIN  1u     /* the scalar reference, one input at a time  */
#define LLM_LOOPS_BASE   2u     /* the vectors every core has: SSE2, or NEON  */
#define LLM_LOOPS_AVX2   3u

#define LLM_GPU_READ  1u        /* read questions on the GPU                   */
#define LLM_GPU_WRITE 2u        /* write answers there                         */

struct llm_settings {
    uint32_t cores;             /* 0: Auto                                     */
    uint32_t loops;             /* LLM_LOOPS_*                                 */
    uint32_t gpu;               /* LLM_GPU_* bits                              */
    uint32_t reserved;
};

struct llm_request {
    uint32_t op;
    uint32_t max_tokens;            /* ASK: at most this many; 0 is 128   */
    uint32_t flags;                 /* ASK: LLM_STOP_*                     */
    uint32_t since;                 /* WAIT: the bytes already seen        */
    uint32_t length;                /* ASK: bytes of `text`, or the region's */
    uint32_t reserved;
    char     name[LLM_NAME_MAX];    /* LOAD: a file in the models folder   */
    char     text[LLM_TEXT_MAX];    /* ASK: the prompt, not terminated     */

    /* ASK: up to two texts that end the answer where they begin, and are
     * not written into it - phi-2's next "\nInstruct:", say. Unused ones
     * are empty; a text of exactly LLM_STOP_MAX bytes has no terminator. */
    char     stop[LLM_STOPS][LLM_STOP_MAX];

    struct llm_settings settings;   /* SETTINGS with LLM_SETTINGS_CHANGE   */
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

    /* The cache, after the last ASK: how many of its prompt's tokens were
     * already held and not read again, and how many it holds now. */
    uint32_t kept_tokens;
    uint32_t held_tokens;

    /* SETTINGS, and every reply: the settings as they stand, and what this
     * machine has to choose from. `loops_have` has bit 1 << LLM_LOOPS_x for
     * each set of loops the processor runs; `gpu_have` the LLM_GPU_* bits
     * there is a GPU for, and `gpu` its name. */
    struct llm_settings settings;
    uint32_t processors, cores;
    uint32_t loops_have, gpu_have;
    char     gpu[32];

    /* What the last answer ran on: the cores it was cut across, its
     * loops, and the LLM_GPU_* bits it used. */
    uint32_t ran_cores, ran_loops, ran_gpu, ran_reserved;
};

_Static_assert(sizeof(struct llm_request) <= 2048, "an LLM request must fit in one message");
_Static_assert(sizeof(struct llm_reply) <= 2048, "an LLM reply must fit in one message");

#endif
