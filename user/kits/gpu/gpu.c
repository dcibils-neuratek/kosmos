/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The GPU Kit** (`kosmos_gpu.h`, `docs/gpu3d.md` G7a): the verbs of
 * `gpuproto.h` for C. One request, one reply, and a capability beside
 * either when the verb carries one - a region to bind, an endpoint to be
 * pulsed, a fence region handed back. Nothing is kept here but the
 * device's capability, asked for once.
 */

#include <stddef.h>
#include <string.h>

#include <kosmos.h>
#include <gpuproto.h>

#include "kosmos_gpu.h"
#include "kits/kitenv.h"

extern lua_State *kosmos_lua_state;

static long device = -2;            /* -2: not asked yet */

long kg_device(void)
{
    if (device == -2) {
        device = kosmos_lua_state != NULL
               ? kit_capability(kosmos_lua_state, "/Devices/gpu") : -1;
    }
    return device;
}

/* One request of `len` bytes and a capability to pass (-1: none); the
 * reply's bytes into `out`, `room` of them, and a capability that came
 * back into `*got` (-1: none). */
static uint32_t ask(long gpu, const void *req, size_t len, long pass,
                    void *out, size_t room, long *got)
{
    /* On the caller's stack, not static: a driver asks from more than one
     * thread - anv's submitting one among them. */
    struct message msg, rep;
    uint32_t error;

    if (got != NULL) {
        *got = -1;
    }
    if (gpu < 0 || len > sizeof msg.data) {
        return KG_NO_ANSWER;
    }

    memset(&msg, 0, sizeof msg);
    msg.length = (uint32_t)len;
    msg.cap_plus_one = pass >= 0 ? (uint32_t)pass + 1u : 0u;
    memcpy(msg.data, req, len);

    if (kosmos_call(gpu, &msg, &rep) != 0 || rep.length < sizeof error) {
        return KG_NO_ANSWER;
    }

    memset(out, 0, room);
    memcpy(out, rep.data, rep.length < room ? rep.length : room);

    if (got != NULL && rep.cap_plus_one != 0) {
        *got = (long)rep.cap_plus_one - 1;
    }

    memcpy(&error, rep.data, sizeof error);
    return error;
}

uint32_t kg_stats(long gpu, struct gpu_stats *out)
{
    struct gpu_request rq;

    memset(&rq, 0, sizeof rq);
    rq.op = GPU_OP_STATS;
    return ask(gpu, &rq, sizeof rq, -1, out, sizeof *out, NULL);
}

static uint32_t ctx_ask(long gpu, const struct gpu_ctx_request *rq, long pass,
                        struct gpu_reply *rep, long *got)
{
    return ask(gpu, rq, sizeof *rq, pass, rep, sizeof *rep, got);
}

uint32_t kg_context_new(long gpu, uint32_t engine, uint32_t *id, long *fence)
{
    struct gpu_ctx_request rq;
    struct gpu_reply rep;
    long got;
    uint32_t e;

    memset(&rq, 0, sizeof rq);
    rq.op = GPU_OP_CONTEXT_NEW;
    rq.engine = engine;
    e = ctx_ask(gpu, &rq, -1, &rep, &got);

    if (e == GPU_OK && got < 0) {
        return KG_NO_ANSWER;        /* a context and no fence to read it by */
    }
    if (e == GPU_OK) {
        *id = rep.words[0];
        *fence = got;
    } else if (got >= 0) {
        (void)kosmos_cap_drop(got);
    }
    return e;
}

uint32_t kg_context_forget(long gpu, uint32_t context)
{
    struct gpu_ctx_request rq;
    struct gpu_reply rep;

    memset(&rq, 0, sizeof rq);
    rq.op = GPU_OP_CONTEXT_FORGET;
    rq.context = context;
    return ctx_ask(gpu, &rq, -1, &rep, NULL);
}

uint32_t kg_bind(long gpu, uint32_t context, uint64_t at, long region,
                 uint32_t first_page, uint32_t pages, uint32_t cache)
{
    struct gpu_ctx_request rq;
    struct gpu_reply rep;

    memset(&rq, 0, sizeof rq);
    rq.op = GPU_OP_BIND;
    rq.context = context;
    rq.gpu = at;
    rq.first_page = first_page;
    rq.pages = pages;
    rq.flags = cache;
    return ctx_ask(gpu, &rq, region, &rep, NULL);
}

uint32_t kg_unbind(long gpu, uint32_t context, uint64_t at)
{
    struct gpu_ctx_request rq;
    struct gpu_reply rep;

    memset(&rq, 0, sizeof rq);
    rq.op = GPU_OP_UNBIND;
    rq.context = context;
    rq.gpu = at;
    return ctx_ask(gpu, &rq, -1, &rep, NULL);
}

uint32_t kg_exec(long gpu, uint32_t context, uint64_t batch, uint32_t bytes,
                 uint64_t signal, const struct kg_wait *waits, uint32_t nwaits)
{
    struct gpu_ctx_request rq;
    struct gpu_reply rep;

    if (nwaits > GPU_EXEC_WAITS) {
        return GPU_ERR_BAD_OP;
    }

    memset(&rq, 0, sizeof rq);
    rq.op = GPU_OP_EXEC;
    rq.context = context;
    rq.gpu = batch;
    rq.pages = bytes;
    rq.signal = signal;
    rq.flags = nwaits;
    for (uint32_t i = 0; i < nwaits; i++) {
        rq.waits[i].context = waits[i].context;
        rq.waits[i].value = waits[i].value;
    }
    return ctx_ask(gpu, &rq, -1, &rep, NULL);
}

uint32_t kg_fence_wait(long gpu, uint32_t context, uint64_t value,
                       long endpoint, uint32_t code)
{
    struct gpu_ctx_request rq;
    struct gpu_reply rep;

    memset(&rq, 0, sizeof rq);
    rq.op = GPU_OP_FENCE_WAIT;
    rq.context = context;
    rq.signal = value;
    rq.flags = code;
    return ctx_ask(gpu, &rq, endpoint, &rep, NULL);
}

uint32_t kg_context_state(long gpu, uint32_t context, struct kg_state *out)
{
    struct gpu_ctx_request rq;
    struct gpu_reply rep;
    uint32_t e;

    memset(&rq, 0, sizeof rq);
    rq.op = GPU_OP_CONTEXT_STATE;
    rq.context = context;
    e = ctx_ask(gpu, &rq, -1, &rep, NULL);

    memset(out, 0, sizeof *out);
    if (e == GPU_OK) {
        out->reached = (uint64_t)rep.words[0] | (uint64_t)rep.words[1] << 32;
        out->given = (uint64_t)rep.words[2] | (uint64_t)rep.words[3] << 32;
        out->hangs = rep.words[4];
        out->faults = rep.words[5];
        out->fault_word = rep.words[6];
    }
    return e;
}

void *kg_fence_map(long fence)
{
    long at = kosmos_mem_map(fence);

    return at < 0 ? NULL : (void *)(uintptr_t)at;
}

const char *kg_why(uint32_t error)
{
    switch (error) {
    case GPU_OK:             return "done";
    case GPU_ERR_NO_DEVICE:  return "no Intel GPU this driver knows";
    case GPU_ERR_BAD_OP:     return "the driver did not take the request";
    case GPU_ERR_NO_MEMORY:  return "the GPU has no room for it";
    case GPU_ERR_BUSY:       return "busy";
    case GPU_ERR_NOT_YOURS:  return "another program's";
    case GPU_ERR_NO_CONTEXT: return "no such context";
    case GPU_ERR_OVERLAP:    return "over a binding already there, or out of range";
    case KG_NO_ANSWER:       return "no GPU's driver answered";
    default:                 return "the GPU's driver refused";
    }
}
