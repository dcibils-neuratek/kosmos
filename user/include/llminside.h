/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_LLMINSIDE_H
#define KOSMOS_LLMINSIDE_H

/*
 * **Inside the model, as the LLM server writes it** (`llmproto.h`'s
 * INSIDE; `docs/xavier.md`, *The LLM Monitor: Inside*): a region the
 * server makes once and hands to every window that asks, holding a ring of
 * the last `LLM_INSIDE_RECORDS` tokens' passages through the model - the
 * LLM Kit's `struct llm_passage`, with the token's text and its candidates'
 * beside it, since a window has no vocabulary of its own.
 *
 * Tokens recur, so they are data in a region and never a message's
 * payload (`CLAUDE.md`, *Control by message, data by shared memory*).
 * **One side writes and the other reads**: the server writes record `seq`
 * at `LLM_INSIDE_DATA + (seq % records) * record_bytes`, then publishes
 * `written = seq + 1` after a barrier (`llm_answer_publish`). A reader
 * loads `written` with acquire, reads the records it has not seen - the
 * last `records` of them at most - and loads `written` again after: a
 * record it read that has since been written over is one it skipped, and
 * it says so. Nothing waits on a reader.
 *
 * **Filled only while watched**: an INSIDE request is a lease of
 * `LLM_INSIDE_LEASE_US`, and a window asks again on its own clock. With
 * nobody watching, the kit is handed nothing to record into and does
 * nothing more.
 */

#include <stdint.h>

#include "kits/llm/llm_core.h"

#define LLM_INSIDE_MAGIC   0x49534e49u    /* "INSI" */
#define LLM_INSIDE_DATA    4096u
#define LLM_INSIDE_RECORDS 256u
#define LLM_INSIDE_TEXT    16u            /* a token's text, cut short, not terminated when full */
#define LLM_INSIDE_LEASE_US 5000000u      /* how long an INSIDE request keeps the ring filled */

struct llm_inside {
    uint32_t magic;
    uint32_t record_bytes;
    uint32_t records;
    uint32_t reserved;
    volatile uint32_t written;            /* records written since the ring was made */
    uint32_t reserved2[3];
};

struct llm_inside_record {
    uint32_t seq;
    uint32_t reading;                     /* 1: a question's token; 0: one the answer wrote */
    uint32_t batch;                       /* how many tokens went through with it          */
    uint32_t us;                          /* the batch's time through the model            */
    uint32_t rows_gpu, rows_all;          /* the batch's products: rows on the GPU, of all */
    uint32_t reserved[2];
    char     text[LLM_INSIDE_TEXT];
    char     cand_text[LLM_PASSAGE_CANDIDATES][LLM_INSIDE_TEXT];
    struct llm_passage p;
};

#define LLM_INSIDE_RECORD_BYTES (32u + LLM_INSIDE_TEXT * (1u + LLM_PASSAGE_CANDIDATES) \
                                 + 104u + 152u * LLM_PASSAGE_LAYERS)
#define LLM_INSIDE_REGION (LLM_INSIDE_DATA + LLM_INSIDE_RECORDS * LLM_INSIDE_RECORD_BYTES)

_Static_assert(sizeof(struct llm_inside) == 32, "llm_inside is 32 bytes");
_Static_assert(__builtin_offsetof(struct llm_inside, written) == 16, "written at 16");
_Static_assert(sizeof(struct llm_inside_record) == LLM_INSIDE_RECORD_BYTES, "a record is as llm.lua reads it");
_Static_assert(__builtin_offsetof(struct llm_inside_record, p) == 208, "the passage at 208");

#endif
