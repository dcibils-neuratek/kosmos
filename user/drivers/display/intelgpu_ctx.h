/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef INTELGPU_CTX_H
#define INTELGPU_CTX_H

/*
 * **Programs' contexts on the GPU** (`docs/gpu3d.md` §5.3, G6e): what the
 * 3D half of `/Devices/gpu` keeps for each program - a context, the regions
 * it bound in the context's own page table at the addresses it chose, its
 * timeline, the work it gave and has not yet been sent, and who asked to be
 * told when the timeline reaches a value. No register is touched here: an
 * engine is a *backend*, handed work once it may run, so all of it runs on
 * the Mac too, held by `tools/test_intelgpu_ctx.c`.
 *
 * Two backends exist. On the M700 the driver's own (`intelgpu.c`): a ring
 * and a context image per context, submitted on execlists. Where there is
 * no Intel GPU - QEMU, and the gate - **the stand-in**, below: the
 * processor running a batch's `MI_` commands itself, through the same page
 * table, so the protocol is held where the hardware is not.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "intelgpu_lrc.h"
#include "gpuproto.h"

/* A run of a region's pages, bound in a context at `gpu`. */
struct igpu_bind {
    uint64_t  gpu;                  /* page-aligned */
    uint32_t  pages;
    uint32_t  first_page;           /* in the region */
    long      cap;                  /* the driver's hold: the pages stay while bound */
    uint64_t *phys;                 /* each page's, as the page table has it */
    uint8_t  *cpu;                  /* the region mapped here, for the stand-in; else NULL */
    unsigned  pat;
};

/* Work given and not yet handed to the engine. */
struct igpu_exec {
    uint64_t batch;
    uint32_t length;                /* bytes */
    uint64_t signal;                /* the timeline's value once it is done */
    uint32_t nwaits;
    struct {
        uint32_t context;
        uint64_t value;
    } waits[GPU_EXEC_WAITS];
};

/* Somebody to tell: a pulse of `code` to `cap` once the timeline reaches
 * `value`, carrying the value reached. */
struct igpu_wait {
    long     cap;
    uint32_t code;
    uint64_t value;
};

struct igpu_ctx {
    bool               used;
    uint32_t           holder;      /* the process that made it */
    uint32_t           engine;      /* GPU_ENGINE_* */
    struct igpu_ppgtt  pt;
    uint32_t           pt_generation;   /* bumped by every unmapping, for the engine's TLB */

    struct igpu_bind  *binds;
    unsigned           nbinds, room_binds;

    struct igpu_exec  *queue;       /* in order: only the first may go */
    unsigned           nqueue, room_queue;
    uint64_t           queued;      /* the last value an accepted exec will signal */
    volatile uint64_t *fence;       /* the value reached, where the engine writes it */

    struct igpu_wait  *waits;
    unsigned           nwaits, room_waits;

    uint32_t           hangs;       /* work that ran too long, and was stopped */
    uint32_t           faults;      /* a batch the engine refused */
    uint64_t           fault_at;    /* the last: where, and the dword */
    uint32_t           fault_word;
    uint32_t           gpr[32];     /* the stand-in's registers, kept across runs */

    void              *hw;          /* the backend's own */
};

/*
 * An engine, to the contexts: `setup` gives a new context its page table,
 * its fence and whatever the engine needs - a ring and an image - and
 * `teardown` takes them back; `give` hands it one exec whose waits are met,
 * or says false while there is no room for it, and it is offered again
 * from `igpu_ctxs_poll`. `map` maps a bound region for a backend that reads
 * memory itself, the stand-in's. The rest is the kernel's, passed in so
 * that the host test has none.
 */
struct igpu_ctx_backend {
    void *env;
    bool (*setup)(void *env, struct igpu_ctx *c, long *fence_cap);
    void (*teardown)(void *env, struct igpu_ctx *c);
    bool (*give)(void *env, struct igpu_ctx *c, const struct igpu_exec *x);
    uint8_t *(*map)(void *env, long cap);
    void (*unmap)(void *env, uint8_t *cpu, long cap);
    long (*page_phys)(long cap, unsigned long index);
    long (*region_pages)(long cap);
    long (*pulse)(long cap, uint32_t code, uint64_t value);
    long (*drop)(long cap);
};

struct igpu_ctxs {
    struct igpu_ctx               *list;    /* id n is list[n - 1] */
    unsigned                       n;
    const struct igpu_ctx_backend *be;
};

/* The value a context's timeline has reached. */
uint64_t igpu_ctx_reached(const struct igpu_ctx *c);

/* Context `id` when `holder` holds it, or NULL with `*error` set. */
struct igpu_ctx *igpu_ctx_get(struct igpu_ctxs *s, uint32_t id, uint32_t holder, uint32_t *error);

/* A new context on `engine` for `holder`: its id, and the read-only
 * capability to its fence page the caller is handed in `*fence_cap`. */
uint32_t igpu_ctx_new(struct igpu_ctxs *s, uint32_t holder, uint32_t engine,
                      uint32_t *id, long *fence_cap);

/* Let go: refused while work it was given is not done. */
uint32_t igpu_ctx_forget(struct igpu_ctxs *s, uint32_t id, uint32_t holder);

/* A region's pages `first_page` on, `pages` of them (0: the rest), bound at
 * `gpu` with caching GPU_CACHE_*. The capability becomes the context's on
 * success; on failure it is still the caller's. */
uint32_t igpu_ctx_bind(struct igpu_ctxs *s, uint32_t id, uint32_t holder, uint64_t gpu,
                       long cap, uint32_t first_page, uint32_t pages, uint32_t cache);

/* The binding that begins at `gpu` undone, once the context is idle. */
uint32_t igpu_ctx_unbind(struct igpu_ctxs *s, uint32_t id, uint32_t holder, uint64_t gpu);

/* A batch at `x->batch`, after `x`'s waits, signalling `x->signal`. */
uint32_t igpu_ctx_exec(struct igpu_ctxs *s, uint32_t id, uint32_t holder, const struct igpu_exec *x);

/* A pulse to `cap` with `code` once the timeline reaches `value` - at once
 * if it already has. The capability is the context's on success. */
uint32_t igpu_ctx_fence_wait(struct igpu_ctxs *s, uint32_t id, uint32_t holder, uint64_t value,
                             long cap, uint32_t code);

/* What has happened since the last time: pulses sent for values reached,
 * and work whose waits are met handed to the engine. Answers whether
 * anything is still waiting - work, or somebody to tell. */
bool igpu_ctxs_poll(struct igpu_ctxs *s);

/*
 * **The stand-in**: a batch run by the processor, through the context's
 * own page table, one command at a time. What it knows is what a program's
 * batch may say that needs no 3D pipeline - MI_NOOP, MI_ARB_CHECK,
 * MI_BATCH_BUFFER_END and _START (chained, or one level down), and
 * MI_STORE_DWORD_IMM, MI_LOAD_REGISTER_IMM, MI_STORE_REGISTER_MEM and
 * MI_LOAD_REGISTER_MEM on the engine's general registers, which are the
 * context's and kept across its runs, as the hardware keeps them in the
 * image. Anything else is a fault, as is an address nothing is bound at or
 * the global GTT, which a program's batch may not name; and a batch still
 * running after IGPU_STANDIN_STEPS commands has hung.
 */
#define IGPU_STANDIN_STEPS (1u << 16)

enum igpu_standin_result {
    IGPU_STANDIN_DONE,
    IGPU_STANDIN_FAULT,
    IGPU_STANDIN_HANG,
};

enum igpu_standin_result igpu_standin_run(struct igpu_ctx *c, uint64_t batch, uint32_t length);

/* The register base the stand-in's general registers are at, by engine:
 * the render engine's 0x2000, the blitter's 0x22000 (CS_GPR, base + 0x600). */
uint32_t igpu_engine_base(uint32_t engine);

#endif
