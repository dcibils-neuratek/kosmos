/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Programs' contexts on the GPU and the stand-in engine, held on the Mac
 * (`user/drivers/display/intelgpu_ctx.h`, `docs/gpu3d.md` G6e).
 *
 * The backend is this file's: regions are pages of the Mac's memory with
 * made-up physical addresses, pulses are written down, and an engine can
 * be told to refuse work, as one that is busy does. Every command a batch
 * holds is worked out by hand from `intel_gpu_commands.h` (Linux 6.12) -
 * none comes from the code under test.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user/drivers/display/intelgpu_ctx.h"

static int failures, checks;

#define CHECK(cond, ...) do {                                   \
        checks++;                                               \
        if (!(cond)) {                                          \
            failures++;                                         \
            printf("FAIL: " __VA_ARGS__);                       \
            printf("\n");                                       \
        }                                                       \
    } while (0)

/* Commands: MI_INSTR(opcode, length) is opcode << 23 | length. */
#define MI_NOOP          0x00000000u
#define MI_BB_END        (0x0au << 23)
#define MI_STORE_DW      ((0x20u << 23) | 2u)          /* four dwords */
#define MI_STORE_QW      ((0x20u << 23) | 3u)          /* five */
#define MI_GGTT          (1u << 22)
#define MI_LRI(n)        ((0x22u << 23) | (2u * (n) - 1u))
#define MI_SRM           ((0x24u << 23) | 2u)
#define MI_LRM           ((0x29u << 23) | 2u)
#define MI_BB_START      ((0x31u << 23) | (1u << 8) | 1u)   /* from the page table */
#define MI_BB_START_2ND  (MI_BB_START | (1u << 22))
#define PIPE_CONTROL     0x7a000004u                    /* a 3D command: not the stand-in's */
#define CS_GPR0          0x2600u                        /* the render engine's */
#define CTX_CONTROL      0x2244u

/*---- the backend: regions, pulses, an engine that can be busy ----*/

#define REGIONS 64

static struct {
    uint8_t *mem;
    unsigned pages;
    int      drops;
} R[REGIONS];
static int nregions;

static struct { long cap; uint32_t code; uint64_t value; } P[64];
static int npulses;

static struct igpu_ctxs S;
static bool hold[16];                                   /* by context id: the engine refuses it */
static int gave;

static long new_region(unsigned pages)
{
    long cap = nregions++;

    R[cap].mem = aligned_alloc(4096, (size_t)pages * 4096u);
    memset(R[cap].mem, 0, (size_t)pages * 4096u);
    R[cap].pages = pages;
    return cap;
}

static uint32_t *words(long cap)
{
    return (uint32_t *)R[cap].mem;
}

static uint64_t phys_of(long cap, unsigned long page)
{
    return ((uint64_t)(cap + 1) << 24) + page * 4096u;
}

static long page_phys(long cap, unsigned long index)
{
    return cap >= 0 && cap < nregions && index < R[cap].pages ? (long)phys_of(cap, index) : -1;
}

static long region_pages(long cap)
{
    return cap >= 0 && cap < nregions ? (long)R[cap].pages : -1;
}

static uint8_t *map(void *env, long cap)
{
    (void)env;
    return R[cap].mem;
}

static long pulse(long cap, uint32_t code, uint64_t value)
{
    P[npulses].cap = cap;
    P[npulses].code = code;
    P[npulses].value = value;
    npulses++;
    return 0;
}

static long drop(long cap)
{
    if (cap >= 0 && cap < nregions) {
        R[cap].drops++;
    }
    return 0;
}

/* A page table's pages, from the Mac's memory. */
static struct { void *cpu; uint64_t phys; } T[4096];
static int ntables;

static void *pt_page(void *ctx, uint64_t *phys)
{
    (void)ctx;
    T[ntables].cpu = aligned_alloc(4096, 4096);
    memset(T[ntables].cpu, 0, 4096);
    T[ntables].phys = 0x7000000000ull + (uint64_t)ntables * 4096u;
    *phys = T[ntables].phys;
    return T[ntables++].cpu;
}

static void *pt_cpu(void *ctx, uint64_t phys)
{
    int i;

    (void)ctx;
    for (i = 0; i < ntables; i++) {
        if (T[i].phys == phys) {
            return T[i].cpu;
        }
    }
    return NULL;
}

static int teardowns;

static bool setup(void *env, struct igpu_ctx *c, long *fence_cap)
{
    struct igpu_pt_pages pages = { pt_page, pt_cpu, NULL };
    long cap = new_region(1);

    (void)env;
    c->fence = (volatile uint64_t *)R[cap].mem;
    *fence_cap = cap;
    return igpu_ppgtt_init(&c->pt, &pages);
}

static void teardown(void *env, struct igpu_ctx *c)
{
    (void)env;
    (void)c;
    teardowns++;
}

static bool give(void *env, struct igpu_ctx *c, const struct igpu_exec *x)
{
    unsigned id = (unsigned)(c - S.list) + 1u;

    (void)env;
    if (hold[id]) {
        return false;
    }

    (void)igpu_standin_run(c, x->batch, x->length);
    *c->fence = x->signal;
    gave++;
    return true;
}

static const struct igpu_ctx_backend BE = {
    NULL, setup, teardown, give, map, NULL, page_phys, region_pages, pulse, drop,
};

/*---- batches ----*/

#define BUF   0x100000000ull                            /* where each context binds its buffer */
#define BATCH 0x000200000ull                            /*            and its batch */

static uint32_t exec(uint32_t id, uint32_t holder, uint64_t signal, uint32_t length,
                     uint32_t wait_ctx, uint64_t wait_value)
{
    struct igpu_exec x;

    memset(&x, 0, sizeof x);
    x.batch = BATCH;
    x.length = length;
    x.signal = signal;
    if (wait_ctx != 0) {
        x.nwaits = 1;
        x.waits[0].context = wait_ctx;
        x.waits[0].value = wait_value;
    }
    return igpu_ctx_exec(&S, id, holder, &x);
}

int main(void)
{
    const uint32_t ME = 7, OTHER = 8;
    uint32_t a, b, c3, err;
    long fa, fb, fc, bufa, bufb, bata, batb, junk;
    uint32_t *wa, *wb, *ba, *bb;

    S.be = &BE;

    /*---- contexts: whose, and which ----*/
    CHECK(igpu_ctx_new(&S, ME, GPU_ENGINE_RENDER, &a, &fa) == GPU_OK && a == 1, "the first context is not 1");
    CHECK(igpu_ctx_new(&S, ME, GPU_ENGINE_RENDER, &b, &fb) == GPU_OK && b == 2, "the second context is not 2");
    CHECK(igpu_ctx_new(&S, ME, 9, &c3, &fc) == GPU_ERR_BAD_OP, "an engine that does not exist was given");
    CHECK(igpu_ctx_get(&S, a, OTHER, &err) == NULL && err == GPU_ERR_NOT_YOURS,
          "another program reached context 1");
    CHECK(igpu_ctx_get(&S, 9, ME, &err) == NULL && err == GPU_ERR_NO_CONTEXT, "context 9 was found");
    CHECK(igpu_ctx_get(&S, 0, ME, &err) == NULL && err == GPU_ERR_NO_CONTEXT, "context 0 was found");

    /*---- bindings: the same address in each, a page of its own ----*/
    bufa = new_region(2);
    bufb = new_region(2);
    bata = new_region(1);
    batb = new_region(1);
    wa = words(bufa);
    wb = words(bufb);
    ba = words(bata);
    bb = words(batb);

    CHECK(igpu_ctx_bind(&S, a, ME, BUF, bufa, 0, 0, GPU_CACHE_WB) == GPU_OK, "context 1's buffer was not bound");
    CHECK(igpu_ctx_bind(&S, b, ME, BUF, bufb, 0, 0, GPU_CACHE_WB) == GPU_OK, "context 2's buffer was not bound");
    CHECK(igpu_ctx_bind(&S, a, ME, BATCH, bata, 0, 1, GPU_CACHE_WB) == GPU_OK, "context 1's batch was not bound");
    CHECK(igpu_ctx_bind(&S, b, ME, BATCH, batb, 0, 1, GPU_CACHE_WB) == GPU_OK, "context 2's batch was not bound");
    CHECK(igpu_ctx_bind(&S, a, OTHER, BUF + 0x10000, bufb, 0, 0, 0) == GPU_ERR_NOT_YOURS,
          "another program bound in context 1");

    junk = new_region(4);
    CHECK(igpu_ctx_bind(&S, a, ME, BUF + 4096, junk, 0, 0, 0) == GPU_ERR_OVERLAP,
          "a binding over another was taken");
    CHECK(igpu_ctx_bind(&S, a, ME, BUF - 4096, junk, 0, 2, 0) == GPU_ERR_OVERLAP,
          "a binding running into another was taken");
    CHECK(igpu_ctx_bind(&S, a, ME, 0x5000 + 8, junk, 0, 0, 0) == GPU_ERR_BAD_OP,
          "a binding off a page boundary was taken");
    CHECK(igpu_ctx_bind(&S, a, ME, (1ull << 48) - 4096, junk, 0, 2, 0) == GPU_ERR_OVERLAP,
          "a binding past 2^48 was taken");
    CHECK(igpu_ctx_bind(&S, a, ME, 0x5000, junk, 4, 0, 0) == GPU_ERR_BAD_OP,
          "a binding from past the region's end was taken");
    CHECK(igpu_ctx_bind(&S, a, ME, 0x5000, junk, 2, 3, 0) == GPU_ERR_BAD_OP,
          "a binding longer than the region was taken");
    CHECK(R[junk].drops == 0, "a refused binding let go of the caller's region");

    {
        uint64_t phys;
        unsigned pat;

        CHECK(igpu_ppgtt_lookup(&S.list[0].pt, BUF + 4096, &phys, &pat) && phys == phys_of(bufa, 1),
              "context 1's page table does not map its buffer's second page there");
        CHECK(igpu_ppgtt_lookup(&S.list[1].pt, BUF, &phys, &pat) && phys == phys_of(bufb, 0),
              "context 2's page table does not map its own buffer");
    }

    /*---- a run each: a register set, a number stored, at the same address ----*/
    ba[0] = MI_LRI(1); ba[1] = CS_GPR0; ba[2] = 0xa0a0a0a0u;
    ba[3] = MI_STORE_DW; ba[4] = (uint32_t)BUF; ba[5] = (uint32_t)(BUF >> 32); ba[6] = 0x11111111u;
    ba[7] = MI_BB_END;
    bb[0] = MI_LRI(1); bb[1] = CS_GPR0; bb[2] = 0xb0b0b0b0u;
    bb[3] = MI_STORE_DW; bb[4] = (uint32_t)BUF; bb[5] = (uint32_t)(BUF >> 32); bb[6] = 0x22222222u;
    bb[7] = MI_BB_END;

    CHECK(exec(a, ME, 1, 32, 0, 0) == GPU_OK, "context 1's first batch was refused");
    CHECK(exec(b, ME, 1, 32, 0, 0) == GPU_OK, "context 2's first batch was refused");
    CHECK(wa[0] == 0x11111111u && wb[0] == 0x22222222u,
          "each context's store did not land in its own page: %08x %08x", wa[0], wb[0]);
    CHECK(igpu_ctx_reached(&S.list[0]) == 1 && igpu_ctx_reached(&S.list[1]) == 1,
          "the timelines did not reach 1");
    CHECK(S.list[0].faults == 0 && S.list[1].faults == 0, "a plain batch faulted");

    /*---- the register kept across runs, context 2's in between ----*/
    memset(ba, 0, 4096);
    ba[0] = MI_SRM; ba[1] = CS_GPR0; ba[2] = (uint32_t)(BUF + 4); ba[3] = (uint32_t)((BUF + 4) >> 32);
    ba[4] = MI_BB_END;
    CHECK(exec(a, ME, 2, 20, 0, 0) == GPU_OK, "context 1's second batch was refused");
    CHECK(wa[1] == 0xa0a0a0a0u, "context 1's register was not kept across a run: %08x", wa[1]);
    CHECK(wb[1] == 0, "context 1's store reached context 2's page");

    /*---- the timeline: no going back, and a pulse for a value reached ----*/
    CHECK(exec(a, ME, 2, 20, 0, 0) == GPU_ERR_BAD_OP, "a value not past the last was taken");
    CHECK(exec(a, ME, 3, 0, 0, 0) == GPU_ERR_BAD_OP, "an empty batch was taken");
    CHECK(exec(a, ME, 3, 22, 0, 0) == GPU_ERR_BAD_OP, "a batch of no whole dwords was taken");
    {
        struct igpu_exec x;

        memset(&x, 0, sizeof x);
        x.batch = 0x900000;
        x.length = 8;
        x.signal = 3;
        CHECK(igpu_ctx_exec(&S, a, ME, &x) == GPU_ERR_BAD_OP, "a batch where nothing is bound was taken");
        x.batch = BATCH + 4092;
        CHECK(igpu_ctx_exec(&S, a, ME, &x) == GPU_ERR_BAD_OP, "a batch running past its binding was taken");
    }

    {
        long ep = 50;

        npulses = 0;
        CHECK(igpu_ctx_fence_wait(&S, a, ME, 2, ep, 0x77) == GPU_OK && npulses == 1 && P[0].cap == ep
              && P[0].code == 0x77 && P[0].value == 2,
              "a wait for a value already reached was not pulsed at once");
        CHECK(igpu_ctx_fence_wait(&S, a, ME, 3, ep, 0x77) == GPU_ERR_BAD_OP,
              "a wait for a value nobody was given was taken");
    }

    /*---- the engine busy: work and a wait held, then let go in order ----*/
    memset(ba, 0, 4096);
    ba[0] = MI_STORE_DW; ba[1] = (uint32_t)(BUF + 8); ba[2] = (uint32_t)((BUF + 8) >> 32); ba[3] = 0x33333333u;
    ba[4] = MI_BB_END;
    memset(bb, 0, 4096);
    bb[0] = MI_STORE_DW; bb[1] = (uint32_t)(BUF + 8); bb[2] = (uint32_t)((BUF + 8) >> 32); bb[3] = 0x44444444u;
    bb[4] = MI_BB_END;

    hold[a] = true;
    npulses = 0;
    gave = 0;
    CHECK(exec(a, ME, 3, 20, 0, 0) == GPU_OK, "context 1's held batch was refused");
    CHECK(exec(b, ME, 2, 20, a, 4) == GPU_ERR_BAD_OP, "a wait for a value context 1 was not given was taken");
    CHECK(exec(b, ME, 2, 20, a, 3) == GPU_OK, "context 2's batch after context 1's was refused");
    CHECK(igpu_ctx_fence_wait(&S, b, ME, 2, 51, 0x99) == GPU_OK && npulses == 0,
          "a wait for a value not reached was pulsed");
    CHECK(gave == 0 && wa[2] == 0 && wb[2] == 0, "work ran while it had to wait");
    CHECK(igpu_ctxs_poll(&S) && gave == 0, "the poll ran work the engine refused, or said nothing waits");
    CHECK(igpu_ctx_unbind(&S, a, ME, BUF) == GPU_ERR_BUSY, "a binding was undone while its work waited");
    CHECK(igpu_ctx_forget(&S, b, ME) == GPU_ERR_BUSY, "a context was let go while its work waited");

    hold[a] = false;
    (void)igpu_ctxs_poll(&S);
    CHECK(gave == 2 && wa[2] == 0x33333333u && wb[2] == 0x44444444u,
          "the held work did not run once let go: %d, %08x %08x", gave, wa[2], wb[2]);
    CHECK(npulses == 1 && P[0].cap == 51 && P[0].code == 0x99 && P[0].value == 2,
          "context 2's waiter was not pulsed once its value came");
    CHECK(!igpu_ctxs_poll(&S), "the poll says something waits when nothing does");

    /*---- what the stand-in refuses ----*/
    {
        static const struct { uint32_t dw[6]; uint32_t n; const char *what; } bad[] = {
            { { PIPE_CONTROL, 0, 0, 0, 0, 0 }, 6, "a 3D command" },
            { { MI_STORE_DW | MI_GGTT, 0x1000, 0, 1, MI_BB_END, 0 }, 5, "a store to the global GTT" },
            { { MI_STORE_DW, 0x900000, 0, 1, MI_BB_END, 0 }, 5, "a store where nothing is bound" },
            { { MI_LRI(1), CTX_CONTROL, 1, MI_BB_END, 0, 0 }, 4, "a register that is not a general one" },
            { { MI_NOOP, MI_NOOP, 0, 0, 0, 0 }, 2, "a batch with no end" },
            { { MI_SRM | MI_GGTT, CS_GPR0, 0x1000, 0, MI_BB_END, 0 }, 5, "a register stored to the global GTT" },
        };
        uint64_t signal = 4;
        unsigned i, k;

        for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            uint32_t before = S.list[0].faults;

            memset(ba, 0, 4096);
            for (k = 0; k < bad[i].n; k++) {
                ba[k] = bad[i].dw[k];
            }
            CHECK(exec(a, ME, signal, bad[i].n * 4u, 0, 0) == GPU_OK, "%s was not given", bad[i].what);
            CHECK(S.list[0].faults == before + 1u, "%s did not fault", bad[i].what);
            CHECK(igpu_ctx_reached(&S.list[0]) == signal, "%s left the timeline behind", bad[i].what);
            signal++;
        }

        CHECK(S.list[0].fault_word == (MI_SRM | MI_GGTT) && S.list[0].fault_at == BATCH,
              "the last fault's dword and address are not said: %08x", S.list[0].fault_word);

        /*---- a batch that jumps to itself, stopped ----*/
        memset(ba, 0, 4096);
        ba[0] = MI_NOOP; ba[1] = MI_BB_START; ba[2] = (uint32_t)BATCH; ba[3] = (uint32_t)(BATCH >> 32);
        CHECK(exec(a, ME, signal, 16, 0, 0) == GPU_OK && S.list[0].hangs == 1
              && igpu_ctx_reached(&S.list[0]) == signal,
              "a batch jumping to itself was not stopped as a hang");
        signal++;

        /*---- one level down and back, a qword, and a register loaded from memory ----*/
        memset(ba, 0, 4096);
        wa[4] = 0x5a5a5a5au;
        ba[0] = MI_BB_START_2ND; ba[1] = (uint32_t)(BATCH + 0x100); ba[2] = (uint32_t)(BATCH >> 32);
        ba[3] = MI_LRM; ba[4] = CS_GPR0 + 8; ba[5] = (uint32_t)(BUF + 16); ba[6] = (uint32_t)(BUF >> 32);
        ba[7] = MI_SRM; ba[8] = CS_GPR0 + 8; ba[9] = (uint32_t)(BUF + 20); ba[10] = (uint32_t)(BUF >> 32);
        ba[11] = MI_BB_END;
        ba[64] = MI_STORE_QW; ba[65] = (uint32_t)(BUF + 24); ba[66] = (uint32_t)(BUF >> 32);
        ba[67] = 0x01234567u; ba[68] = 0x89abcdefu;
        ba[69] = MI_BB_END;
        CHECK(exec(a, ME, signal, 48, 0, 0) == GPU_OK, "the two-level batch was refused");
        CHECK(wa[6] == 0x01234567u && wa[7] == 0x89abcdefu, "the batch one level down did not store its qword");
        CHECK(wa[5] == 0x5a5a5a5au, "the batch did not come back up, or load the register: %08x", wa[5]);
        CHECK(S.list[0].faults == sizeof bad / sizeof bad[0], "the two-level batch faulted");
        signal++;

        /*---- the stand-in reads through the page table, not around it ----*/
        CHECK(igpu_ppgtt_map(&S.list[0].pt, BUF, phys_of(bufb, 0), IGPU_PAT_WB_LLC),
              "the control's entry could not be written");
        memset(ba, 0, 4096);
        ba[0] = MI_STORE_DW; ba[1] = (uint32_t)BUF; ba[2] = (uint32_t)(BUF >> 32); ba[3] = 0x66666666u;
        ba[4] = MI_BB_END;
        CHECK(exec(a, ME, signal, 20, 0, 0) == GPU_OK
              && S.list[0].faults == sizeof bad / sizeof bad[0] + 1u && wa[0] == 0x11111111u
              && wb[0] == 0x22222222u,
              "a store through an entry the binding does not say was not refused");
        CHECK(igpu_ppgtt_map(&S.list[0].pt, BUF, phys_of(bufa, 0), IGPU_PAT_WB_LLC), "the entry was not put back");
        signal++;
    }

    /*---- undone and let go ----*/
    CHECK(igpu_ctx_unbind(&S, a, ME, BUF + 4096) == GPU_ERR_NO_BUFFER, "a binding nothing begins at was undone");
    CHECK(igpu_ctx_unbind(&S, a, ME, BUF) == GPU_OK && R[bufa].drops == 1, "context 1's buffer was not let go");
    {
        uint64_t phys;
        unsigned pat;

        CHECK(!igpu_ppgtt_lookup(&S.list[0].pt, BUF, &phys, &pat), "an undone binding is still in the page table");
    }
    CHECK(igpu_ctx_forget(&S, a, OTHER) == GPU_ERR_NOT_YOURS, "another program let go of context 1");
    CHECK(igpu_ctx_forget(&S, a, ME) == GPU_OK && R[bata].drops == 1 && teardowns == 1,
          "context 1 was not let go with its batch");
    CHECK(igpu_ctx_get(&S, a, ME, &err) == NULL && err == GPU_ERR_NO_CONTEXT, "a context let go is still found");
    CHECK(igpu_ctx_new(&S, ME, GPU_ENGINE_COPY, &c3, &fc) == GPU_OK && c3 == a,
          "a context's id was not given again once let go");
    CHECK(S.list[c3 - 1u].nbinds == 0 && igpu_ctx_reached(&S.list[c3 - 1u]) == 0,
          "a new context kept its predecessor's state");

    /*---- many contexts: no count compiled in ----*/
    {
        uint32_t id = 0, last = 0;
        long f;
        int i, made = 0;

        for (i = 0; i < 40; i++) {
            if (igpu_ctx_new(&S, OTHER, GPU_ENGINE_RENDER, &id, &f) == GPU_OK) {
                made++;
                last = id;
            }
        }
        CHECK(made == 40 && last == 42, "forty more contexts were not made: %d, the last %u", made, last);
        CHECK(igpu_ctx_get(&S, b, ME, &err) != NULL && S.list[b - 1u].binds[0].gpu == BUF,
              "a context's bindings did not survive the list growing");
    }

    printf("%s: %d of %d checks on programs' contexts and the stand-in engine\n",
           failures == 0 ? "PASS" : "FAIL", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
