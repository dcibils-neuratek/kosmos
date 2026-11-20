/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Programs' contexts on the GPU, and the stand-in engine (`intelgpu_ctx.h`,
 * `docs/gpu3d.md` G6e). Memory and bookkeeping only: what touches the GPU
 * is the backend's.
 */

#include "intelgpu_ctx.h"

#include <stdlib.h>
#include <string.h>

#define PAGE 4096u
#define GPU_LIMIT (1ull << 48)          /* a 48-bit, four-level page table */

/* Room for one more in an array that grows by doubling - the driver has no
 * compiled-in count of contexts, bindings or waits. */
static bool grow(void **list, unsigned *room, unsigned used, size_t size)
{
    void *bigger;
    unsigned want;

    if (used < *room) {
        return true;
    }

    want = *room == 0 ? 8u : *room * 2u;
    if (want <= *room || (bigger = realloc(*list, (size_t)want * size)) == NULL) {
        return false;
    }

    *list = bigger;
    *room = want;
    return true;
}

uint32_t igpu_engine_base(uint32_t engine)
{
    return engine == GPU_ENGINE_COPY ? 0x22000u : 0x2000u;
}

uint64_t igpu_ctx_reached(const struct igpu_ctx *c)
{
    return c->fence != NULL ? *c->fence : 0;
}

struct igpu_ctx *igpu_ctx_get(struct igpu_ctxs *s, uint32_t id, uint32_t holder, uint32_t *error)
{
    struct igpu_ctx *c;

    if (id == 0 || id > s->n || !s->list[id - 1u].used) {
        *error = GPU_ERR_NO_CONTEXT;
        return NULL;
    }

    c = &s->list[id - 1u];

    if (c->holder != holder || holder == 0) {
        *error = GPU_ERR_NOT_YOURS;
        return NULL;
    }

    return c;
}

uint32_t igpu_ctx_new(struct igpu_ctxs *s, uint32_t holder, uint32_t engine,
                      uint32_t *id, long *fence_cap)
{
    struct igpu_ctx *c;
    unsigned i;

    if (engine > GPU_ENGINE_COPY || holder == 0) {
        return GPU_ERR_BAD_OP;
    }

    for (i = 0; i < s->n && s->list[i].used; i++) {
    }

    if (i == s->n) {
        unsigned room = s->n;

        if (!grow((void **)&s->list, &room, s->n, sizeof *s->list)) {
            return GPU_ERR_NO_MEMORY;
        }

        /* The list is as long as its room: a free slot is one not used. */
        memset(&s->list[s->n], 0, (size_t)(room - s->n) * sizeof *s->list);
        s->n = room;
    }

    c = &s->list[i];
    memset(c, 0, sizeof *c);
    c->holder = holder;
    c->engine = engine;
    *fence_cap = -1;

    if (!s->be->setup(s->be->env, c, fence_cap)) {
        memset(c, 0, sizeof *c);
        return GPU_ERR_NO_MEMORY;
    }

    c->used = true;
    *id = i + 1u;
    return GPU_OK;
}

/* Whether everything given has been done. */
static bool idle(const struct igpu_ctx *c)
{
    return c->nqueue == 0 && igpu_ctx_reached(c) >= c->queued;
}

static void bind_release(struct igpu_ctxs *s, struct igpu_ctx *c, struct igpu_bind *b)
{
    uint32_t i;

    for (i = 0; i < b->pages; i++) {
        igpu_ppgtt_unmap(&c->pt, b->gpu + (uint64_t)i * PAGE);
    }

    c->pt_generation++;

    if (b->cpu != NULL && s->be->unmap != NULL) {
        s->be->unmap(s->be->env, b->cpu, b->cap);
    }

    (void)s->be->drop(b->cap);
    free(b->phys);
}

uint32_t igpu_ctx_forget(struct igpu_ctxs *s, uint32_t id, uint32_t holder)
{
    uint32_t error;
    struct igpu_ctx *c = igpu_ctx_get(s, id, holder, &error);
    unsigned i;

    if (c == NULL) {
        return error;
    }

    if (!idle(c)) {
        return GPU_ERR_BUSY;
    }

    for (i = 0; i < c->nbinds; i++) {
        bind_release(s, c, &c->binds[i]);
    }

    /* Nobody can still be waiting for a value: one past what was given is
     * refused, and the rest were told as the timeline reached them. */
    for (i = 0; i < c->nwaits; i++) {
        (void)s->be->drop(c->waits[i].cap);
    }

    s->be->teardown(s->be->env, c);
    free(c->binds);
    free(c->queue);
    free(c->waits);
    memset(c, 0, sizeof *c);
    return GPU_OK;
}

static unsigned pat_of(uint32_t cache)
{
    return cache == GPU_CACHE_UC ? IGPU_PAT_UC : cache == GPU_CACHE_WB_ELLC ? IGPU_PAT_WB_LLCELLC
                                                                           : IGPU_PAT_WB_LLC;
}

uint32_t igpu_ctx_bind(struct igpu_ctxs *s, uint32_t id, uint32_t holder, uint64_t gpu,
                       long cap, uint32_t first_page, uint32_t pages, uint32_t cache)
{
    uint32_t error;
    struct igpu_ctx *c = igpu_ctx_get(s, id, holder, &error);
    struct igpu_bind b;
    long total;
    uint32_t i;

    if (c == NULL) {
        return error;
    }

    if (cap < 0 || cache > GPU_CACHE_WB_ELLC || (gpu & (PAGE - 1u)) != 0
        || (total = s->be->region_pages(cap)) <= 0 || first_page >= (unsigned long)total) {
        return GPU_ERR_BAD_OP;
    }

    if (pages == 0) {
        pages = (uint32_t)((unsigned long)total - first_page);
    }

    if (pages > (unsigned long)total - first_page) {
        return GPU_ERR_BAD_OP;
    }

    if (gpu >= GPU_LIMIT || (uint64_t)pages * PAGE > GPU_LIMIT - gpu) {
        return GPU_ERR_OVERLAP;
    }

    for (i = 0; i < c->nbinds; i++) {
        const struct igpu_bind *o = &c->binds[i];

        if (gpu < o->gpu + (uint64_t)o->pages * PAGE && o->gpu < gpu + (uint64_t)pages * PAGE) {
            return GPU_ERR_OVERLAP;
        }
    }

    if (!grow((void **)&c->binds, &c->room_binds, c->nbinds, sizeof *c->binds)) {
        return GPU_ERR_NO_MEMORY;
    }

    memset(&b, 0, sizeof b);
    b.gpu = gpu;
    b.pages = pages;
    b.first_page = first_page;
    b.cap = cap;
    b.pat = pat_of(cache);

    if ((b.phys = malloc((size_t)pages * sizeof *b.phys)) == NULL) {
        return GPU_ERR_NO_MEMORY;
    }

    for (i = 0; i < pages; i++) {
        long phys = s->be->page_phys(cap, (unsigned long)first_page + i);

        b.phys[i] = (uint64_t)phys;

        if (phys < 0 || !igpu_ppgtt_map(&c->pt, gpu + (uint64_t)i * PAGE, b.phys[i], b.pat)) {
            while (i-- > 0) {
                igpu_ppgtt_unmap(&c->pt, gpu + (uint64_t)i * PAGE);
            }

            free(b.phys);
            return GPU_ERR_NO_MEMORY;
        }
    }

    if (s->be->map != NULL && (b.cpu = s->be->map(s->be->env, cap)) == NULL) {
        for (i = 0; i < pages; i++) {
            igpu_ppgtt_unmap(&c->pt, gpu + (uint64_t)i * PAGE);
        }

        free(b.phys);
        return GPU_ERR_NO_MEMORY;
    }

    c->binds[c->nbinds++] = b;
    return GPU_OK;
}

uint32_t igpu_ctx_unbind(struct igpu_ctxs *s, uint32_t id, uint32_t holder, uint64_t gpu)
{
    uint32_t error;
    struct igpu_ctx *c = igpu_ctx_get(s, id, holder, &error);
    unsigned i;

    if (c == NULL) {
        return error;
    }

    for (i = 0; i < c->nbinds && c->binds[i].gpu != gpu; i++) {
    }

    if (i == c->nbinds) {
        return GPU_ERR_NO_BUFFER;
    }

    /* The GPU may still reach the pages while work it was given runs. */
    if (!idle(c)) {
        return GPU_ERR_BUSY;
    }

    bind_release(s, c, &c->binds[i]);
    c->binds[i] = c->binds[--c->nbinds];
    return GPU_OK;
}

/* The binding `gpu` is in, with `bytes` after it in the same one. */
static struct igpu_bind *bound_at(struct igpu_ctx *c, uint64_t gpu, uint64_t bytes)
{
    unsigned i;

    for (i = 0; i < c->nbinds; i++) {
        struct igpu_bind *b = &c->binds[i];
        uint64_t size = (uint64_t)b->pages * PAGE;

        if (gpu >= b->gpu && gpu - b->gpu < size && bytes <= size - (gpu - b->gpu)) {
            return b;
        }
    }

    return NULL;
}

static bool waits_met(const struct igpu_ctxs *s, const struct igpu_exec *x)
{
    uint32_t i;

    for (i = 0; i < x->nwaits; i++) {
        const struct igpu_ctx *w = &s->list[x->waits[i].context - 1u];

        if (w->used && igpu_ctx_reached(w) < x->waits[i].value) {
            return false;
        }
    }

    return true;
}

/* What may go, in order, until something has to wait. */
static void pump(struct igpu_ctxs *s, struct igpu_ctx *c)
{
    while (c->nqueue > 0 && waits_met(s, &c->queue[0]) && s->be->give(s->be->env, c, &c->queue[0])) {
        c->nqueue--;
        memmove(&c->queue[0], &c->queue[1], (size_t)c->nqueue * sizeof *c->queue);
    }
}

uint32_t igpu_ctx_exec(struct igpu_ctxs *s, uint32_t id, uint32_t holder, const struct igpu_exec *x)
{
    uint32_t error, i;
    struct igpu_ctx *c = igpu_ctx_get(s, id, holder, &error);

    if (c == NULL) {
        return error;
    }

    if (x->length == 0 || (x->length & 3u) != 0 || (x->batch & 3u) != 0 || x->signal <= c->queued
        || x->nwaits > GPU_EXEC_WAITS || bound_at(c, x->batch, x->length) == NULL) {
        return GPU_ERR_BAD_OP;
    }

    /* A wait is on a context of the caller's own, for a value that will
     * come: one nobody has been given would wait for ever. */
    for (i = 0; i < x->nwaits; i++) {
        const struct igpu_ctx *w = igpu_ctx_get(s, x->waits[i].context, holder, &error);

        if (w == NULL) {
            return error;
        }

        if (x->waits[i].value > w->queued) {
            return GPU_ERR_BAD_OP;
        }
    }

    if (!grow((void **)&c->queue, &c->room_queue, c->nqueue, sizeof *c->queue)) {
        return GPU_ERR_NO_MEMORY;
    }

    c->queue[c->nqueue++] = *x;
    c->queued = x->signal;
    pump(s, c);
    return GPU_OK;
}

uint32_t igpu_ctx_fence_wait(struct igpu_ctxs *s, uint32_t id, uint32_t holder, uint64_t value,
                             long cap, uint32_t code)
{
    uint32_t error;
    struct igpu_ctx *c = igpu_ctx_get(s, id, holder, &error);
    uint64_t reached;

    if (c == NULL) {
        return error;
    }

    if (cap < 0 || value > c->queued) {
        return GPU_ERR_BAD_OP;
    }

    if ((reached = igpu_ctx_reached(c)) >= value) {
        (void)s->be->pulse(cap, code, reached);
        (void)s->be->drop(cap);
        return GPU_OK;
    }

    if (!grow((void **)&c->waits, &c->room_waits, c->nwaits, sizeof *c->waits)) {
        return GPU_ERR_NO_MEMORY;
    }

    c->waits[c->nwaits].cap = cap;
    c->waits[c->nwaits].code = code;
    c->waits[c->nwaits].value = value;
    c->nwaits++;
    return GPU_OK;
}

bool igpu_ctxs_poll(struct igpu_ctxs *s)
{
    bool busy = false;
    unsigned i, k;

    for (i = 0; i < s->n; i++) {
        struct igpu_ctx *c = &s->list[i];
        uint64_t reached;

        if (!c->used) {
            continue;
        }

        pump(s, c);
        reached = igpu_ctx_reached(c);

        for (k = 0; k < c->nwaits;) {
            if (reached >= c->waits[k].value) {
                (void)s->be->pulse(c->waits[k].cap, c->waits[k].code, reached);
                (void)s->be->drop(c->waits[k].cap);
                c->waits[k] = c->waits[--c->nwaits];
            } else {
                k++;
            }
        }

        busy = busy || c->nqueue > 0 || c->nwaits > 0 || reached < c->queued;
    }

    return busy;
}

/*--------------------------------------------------------------------------
 * **The stand-in** (`intelgpu_ctx.h`).
 *------------------------------------------------------------------------*/

#define MI_OP(dw)           (((dw) >> 23) & 0x3fu)
#define MI_NOOP_OP          0x00u
#define MI_ARB_CHECK_OP     0x05u
#define MI_BB_END_OP        0x0au
#define MI_STORE_DATA_OP    0x20u
#define MI_LRI_OP           0x22u
#define MI_SRM_OP           0x24u
#define MI_LRM_OP           0x29u
#define MI_BB_START_OP      0x31u
#define MI_GGTT             (1u << 22)  /* MI_USE_GGTT, MI_SRM_LRM_GLOBAL_GTT */
#define MI_BB_SECOND_LEVEL  (1u << 22)
#define GPR_AT              0x600u      /* CS_GPR(0): sixteen qwords */
#define GPR_DWORDS          32u

/*
 * Where `gpu` is for the processor: through the context's page table, the
 * walk's answer held to the binding's page - so the stand-in reads only
 * what the table maps, as the engine would - and `bytes` within the page.
 */
static uint8_t *standin_at(struct igpu_ctx *c, uint64_t gpu, unsigned bytes)
{
    struct igpu_bind *b;
    uint64_t phys, page;
    unsigned pat;

    if (gpu >= GPU_LIMIT || (gpu & (PAGE - 1u)) + bytes > PAGE
        || !igpu_ppgtt_lookup(&c->pt, gpu & ~(uint64_t)(PAGE - 1u), &phys, &pat)
        || (b = bound_at(c, gpu, bytes)) == NULL || b->cpu == NULL) {
        return NULL;
    }

    page = (gpu - b->gpu) / PAGE;

    if (b->phys[page] != phys) {
        return NULL;
    }

    return b->cpu + ((uint64_t)b->first_page + page) * PAGE + (gpu & (PAGE - 1u));
}

static bool read32(struct igpu_ctx *c, uint64_t gpu, uint32_t *v)
{
    uint8_t *p = (gpu & 3u) == 0 ? standin_at(c, gpu, 4) : NULL;

    if (p == NULL) {
        return false;
    }

    memcpy(v, p, 4);
    return true;
}

static bool write32(struct igpu_ctx *c, uint64_t gpu, uint32_t v)
{
    uint8_t *p = (gpu & 3u) == 0 ? standin_at(c, gpu, 4) : NULL;

    if (p == NULL) {
        return false;
    }

    memcpy(p, &v, 4);
    return true;
}

/* A general register's index from its address, or -1. */
static int gpr_of(const struct igpu_ctx *c, uint32_t reg)
{
    uint32_t at = igpu_engine_base(c->engine) + GPR_AT;

    reg &= 0x7ffffcu;
    return reg >= at && reg < at + GPR_DWORDS * 4u ? (int)((reg - at) / 4u) : -1;
}

enum igpu_standin_result igpu_standin_run(struct igpu_ctx *c, uint64_t batch, uint32_t length)
{
    uint64_t pc = batch, end = batch + length, back = 0, back_end = 0;
    bool nested = false;
    uint32_t steps, d[4];

    for (steps = 0; steps < IGPU_STANDIN_STEPS; steps++) {
        uint32_t dw, n, i;

        if (pc >= end || !read32(c, pc, &dw) || (dw >> 29) != 0) {
            goto fault;
        }

        n = (dw & 0xffu) + 2u;                      /* a command's dwords, its length field + 2 */

        switch (MI_OP(dw)) {
        case MI_NOOP_OP:
        case MI_ARB_CHECK_OP:
            pc += 4;
            continue;

        case MI_BB_END_OP:
            if (nested) {
                nested = false;
                pc = back;
                end = back_end;
                continue;
            }
            return IGPU_STANDIN_DONE;

        case MI_STORE_DATA_OP:                      /* four dwords, or five for a qword */
            if ((dw & MI_GGTT) != 0 || (n != 4u && n != 5u)) {
                goto fault;
            }
            for (i = 1; i < n; i++) {
                if (!read32(c, pc + 4u * i, &d[i - 1u])) {
                    goto fault;
                }
            }
            {
                uint64_t to = d[0] | (uint64_t)(d[1] & 0xffffu) << 32;

                if (!write32(c, to, d[2]) || (n == 5u && ((to & 7u) != 0 || !write32(c, to + 4u, d[3])))) {
                    goto fault;
                }
            }
            pc += 4u * n;
            continue;

        case MI_LRI_OP:                             /* pairs: a register, its value */
            if ((n & 1u) == 0) {
                goto fault;
            }
            for (i = 1; i < n; i += 2) {
                uint32_t reg, value;
                int g;

                if (!read32(c, pc + 4u * i, &reg) || !read32(c, pc + 4u * (i + 1u), &value)
                    || (g = gpr_of(c, reg)) < 0) {
                    goto fault;
                }
                c->gpr[g] = value;
            }
            pc += 4u * n;
            continue;

        case MI_SRM_OP:                             /* a register, the address low and high */
        case MI_LRM_OP: {
            uint64_t to;
            int g;

            if ((dw & MI_GGTT) != 0 || n != 4u) {
                goto fault;
            }
            for (i = 1; i < 4; i++) {
                if (!read32(c, pc + 4u * i, &d[i - 1u])) {
                    goto fault;
                }
            }
            to = d[1] | (uint64_t)(d[2] & 0xffffu) << 32;

            if ((g = gpr_of(c, d[0])) < 0) {
                goto fault;
            }
            if (MI_OP(dw) == MI_SRM_OP ? !write32(c, to, c->gpr[g]) : !read32(c, to, &c->gpr[g])) {
                goto fault;
            }
            pc += 16;
            continue;
        }

        case MI_BB_START_OP: {                      /* three dwords: the address low and high */
            if (n != 3u || !read32(c, pc + 4, &d[0]) || !read32(c, pc + 8, &d[1])) {
                goto fault;
            }
            if ((dw & MI_BB_SECOND_LEVEL) != 0) {
                if (nested) {
                    goto fault;                     /* one level down, and no further */
                }
                nested = true;
                back = pc + 12;
                back_end = end;
            }
            pc = d[0] | (uint64_t)(d[1] & 0xffffu) << 32;
            end = GPU_LIMIT;                        /* a batch jumped to ends where it says */
            continue;
        }

        default:
            goto fault;
        }

    fault:
        c->faults++;
        c->fault_at = pc;
        c->fault_word = 0;
        (void)read32(c, pc, &c->fault_word);
        return IGPU_STANDIN_FAULT;
    }

    c->hangs++;
    return IGPU_STANDIN_HANG;
}
