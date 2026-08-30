/*
 * The benchmark suite.
 *
 * `testing.md` §18.3 is the reason this is a separate image from the tests
 * rather than a few more of them. Correctness and cost are different
 * questions and they want different instruments: a test asserts, a benchmark
 * measures, and a measurement is only worth anything if it is repeatable.
 *
 * Repeatability comes from QEMU's `-icount`, which makes the emulated clock
 * a function of instructions retired rather than of host time. Measured
 * before relying on it: the same spin loop takes 88187, 88687 and 89062
 * ticks across three ordinary runs, and 75001, 75000, 75001 under `-icount`.
 * That is the difference between a number that can detect a regression and
 * one that cannot.
 *
 * What these numbers are NOT is performance. QEMU is a translator and a tick
 * here has no relationship to a cycle on a Pi. They exist to answer "did
 * this get worse since yesterday", and `testing.md` §18.8 is explicit that
 * optimising against them is wasted effort.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bench.h"
#include "console.h"
#include "thread.h"
#include "ipc.h"
#include "trap.h"
#include "semihosting.h"
#include "serialize.h"
#include "kosmos_lua.h"
#include "lua.h"
#include "lauxlib.h"

static inline uint64_t now(void)
{
    uint64_t t;
    /* isb first, or the read can be reordered ahead of the work being
     * measured. On a machine that reorders as freely as this one, a
     * benchmark without it measures something adjacent to what it claims. */
    __asm__ volatile("isb" ::: "memory");
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
    return t;
}

/*
 * Interrupts are masked around every measurement.
 *
 * At 100 Hz the timer fires often enough to land inside a long run, and
 * where it lands is the one thing `-icount` does not pin down: it is the
 * source of the ±1 tick that survives otherwise. A benchmark of IPC should
 * measure IPC.
 */
static inline uint64_t irq_disable(void)
{
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #2" ::: "memory");
    return daif;
}

static inline void irq_restore(uint64_t daif)
{
    __asm__ volatile("msr daif, %0" : : "r"(daif) : "memory");
}

/* ------------------------------------------------------------------ */

/*
 * The one the milestone is defined by: two threads passing a message back
 * and forth, with the cost of a full round trip reported.
 *
 * Everything the system will be built out of sits on top of this number.
 * Every namespace lookup, every file read, every drawing command from M6 is
 * a round trip, so it is the figure to look at first when anything feels
 * slow.
 */
#define IPC_ROUNDS  100000

static cap_t     ipc_server_cap;
static volatile bool ipc_server_ready;

static void ipc_echo_server(void *arg)
{
    unsigned long rounds = (unsigned long)(uintptr_t)arg;
    unsigned long i;

    ipc_server_ready = true;

    for (i = 0; i < rounds; i++) {
        struct message msg;
        struct thread *sender;

        if (ipc_receive(ipc_server_cap, &msg, &sender) != IPC_OK) {
            return;
        }

        /* The reply is the request. Doing no work is the point: what is
         * being measured is the round trip, not the server. */
        if (ipc_reply(sender, &msg) != IPC_OK) {
            return;
        }
    }
}

static bool bench_ipc_roundtrip(struct bench_result *out)
{
    struct message msg = { 0 };
    struct message reply = { 0 };
    struct thread *server;
    cap_t client_cap;
    uint64_t start;
    uint64_t daif;
    unsigned long i;

    client_cap = ipc_endpoint_create();
    if (client_cap < 0) {
        return false;
    }

    ipc_server_ready = false;

    /* Suspended until its capability is in place. Started before that, it
     * reads whatever ipc_server_cap happened to hold, fails its receive and
     * exits, and the benchmark deadlocks against a server that is gone. */
    server = thread_create_suspended("bench-echo", ipc_echo_server,
                                     (void *)(uintptr_t)IPC_ROUNDS);
    if (server == NULL) {
        return false;
    }

    ipc_server_cap = ipc_cap_grant(server, client_cap);
    if (ipc_server_cap < 0) {
        return false;
    }

    thread_wake(server);

    /* Let it reach its first receive, so the measured loop is all steady
     * state and none of it is startup. */
    while (!ipc_server_ready) {
        thread_yield();
    }
    thread_yield();

    msg.tag = 1;

    daif = irq_disable();
    start = now();

    for (i = 0; i < IPC_ROUNDS; i++) {
        if (ipc_call(client_cap, &msg, &reply) != IPC_OK) {
            irq_restore(daif);
            return false;
        }
    }

    out->total = now() - start;
    irq_restore(daif);

    out->iterations = IPC_ROUNDS;

    (void)ipc_endpoint_destroy(client_cap);
    return true;
}

/* ------------------------------------------------------------------ */

/*
 * A bare context switch, with no IPC on top.
 *
 * The difference between this and the round trip is what the IPC layer
 * itself costs, which is the number to watch when the protocol grows.
 */
#define SWITCH_ROUNDS   100000

static volatile unsigned long switch_partner_rounds;

static void switch_partner(void *arg)
{
    unsigned long rounds = (unsigned long)(uintptr_t)arg;
    unsigned long i;

    for (i = 0; i < rounds; i++) {
        switch_partner_rounds++;
        thread_yield();
    }
}

static bool bench_context_switch(struct bench_result *out)
{
    struct thread *partner;
    uint64_t start;
    uint64_t daif;
    unsigned long i;

    switch_partner_rounds = 0;

    /* One more round than this loop needs, so the partner is never the one
     * that runs out and changes what a yield costs partway through. */
    partner = thread_create("bench-yield", switch_partner,
                            (void *)(uintptr_t)(SWITCH_ROUNDS + 16));
    if (partner == NULL) {
        return false;
    }

    thread_yield();

    daif = irq_disable();
    start = now();

    for (i = 0; i < SWITCH_ROUNDS; i++) {
        thread_yield();
    }

    out->total = now() - start;
    irq_restore(daif);

    /* Each iteration is two switches: out to the partner and back. */
    out->iterations = SWITCH_ROUNDS * 2;
    return true;
}

/* ------------------------------------------------------------------ */

/*
 * Taking an exception and returning from it.
 *
 * The cost of the vector's save and restore, which every fault, every
 * interrupt and, from M4, every syscall pays. `testing.md` §18.4 asks for
 * page fault latency at this milestone; this is that path measured through
 * the machinery that already exists to cause one deliberately.
 */
#define FAULT_ROUNDS    20000

static bool bench_exception(struct bench_result *out)
{
    uint64_t start;
    uint64_t daif;
    unsigned long i;

    daif = irq_disable();
    start = now();

    for (i = 0; i < FAULT_ROUNDS; i++) {
        fault_expect_begin();
        __asm__ volatile("udf #0");
        (void)fault_expect_end(NULL);
    }

    out->total = now() - start;
    irq_restore(daif);

    out->iterations = FAULT_ROUNDS;
    return true;
}

/* ------------------------------------------------------------------ */

/*
 * A typical message, packed and unpacked.
 *
 * `roadmap.md` asks for this at M4, and it is the cost `design.md` §1's
 * thesis is paid for with: the protocol between servers is the data model of
 * the language, and this is what that conversion costs per message. Every
 * namespace read from M5 and every drawing command from M6 pays it twice.
 *
 * "Typical" is a small record with a tag, a couple of strings and a nested
 * table, which is what a namespace operation looks like.
 */
#define SERIALIZE_ROUNDS    20000

static const char typical_chunk[] =
    "return { tag = 3, op = 'read', path = '/dev/temp', "
    "         opts = { follow = true, limit = 64 } }";

/*
 * Reported as the best of several states, not one.
 *
 * Lua randomises string hashing per state, so table iteration order and the
 * collision patterns underneath it differ every boot, and the cost with it.
 * Measured across ten runs the spread is 6.9%, which is far too wide for a
 * 2% tolerance and far too wide to widen the tolerance around: a threshold
 * of 15% would stop detecting anything worth detecting.
 *
 * The minimum across several seeds is the cost with the least unlucky
 * layout, and it is stable because the best case is. Taking the best rather
 * than the mean is the usual answer to noise that only ever adds.
 *
 * Note what is not done: the seed is not fixed for the benchmark. That would
 * measure an image that behaves differently from the one that runs.
 */
#define SERIALIZE_STATES    5

static bool bench_serialize_once(uint64_t *ticks)
{
    lua_State *L = kosmos_lua_open();
    struct message m;
    uint64_t start;
    uint64_t daif;
    unsigned long i;

    if (L == NULL) {
        return false;
    }

    if (kosmos_lua_dostring(L, "=bench", typical_chunk) != LUA_OK) {
        lua_close(L);
        return false;
    }

    daif = irq_disable();
    start = now();

    for (i = 0; i < SERIALIZE_ROUNDS; i++) {
        if (serialize_pack(L, -1, &m) != SERIALIZE_OK
            || serialize_unpack(L, &m) != SERIALIZE_OK) {
            irq_restore(daif);
            lua_close(L);
            return false;
        }

        lua_pop(L, 1);      /* the unpacked copy */
    }

    *ticks = now() - start;
    irq_restore(daif);

    lua_close(L);
    return true;
}

static bool bench_serialize(struct bench_result *out)
{
    uint64_t best = 0;
    unsigned i;

    for (i = 0; i < SERIALIZE_STATES; i++) {
        uint64_t ticks;

        if (!bench_serialize_once(&ticks)) {
            return false;
        }

        if (i == 0 || ticks < best) {
            best = ticks;
        }
    }

    out->total = best;
    out->iterations = SERIALIZE_ROUNDS;
    return true;
}

/*
 * The longest the collector stops for.
 *
 * `design.md` §5.2 calls this the project's recurring problem, and
 * `testing.md` §18.5 is emphatic that it is the maximum that matters and not
 * the average: a system averaging 8 ms a frame with a 40 ms spike every two
 * seconds feels worse than one holding a steady 14 ms.
 *
 * Reported as the worst single step rather than as a rate, which is why
 * iterations is one.
 */
#define GC_STEPS    2000

static const char gc_setup[] =
    "garbage = {} "
    "for i = 1, 3000 do garbage[i] = { i, tostring(i), { i } } end "
    "collectgarbage('setpause', 100)";

static bool bench_gc_pause(struct bench_result *out)
{
    lua_State *L = kosmos_lua_open();
    uint64_t worst = 0;
    uint64_t daif;
    unsigned long i;

    if (L == NULL) {
        return false;
    }

    if (kosmos_lua_dostring(L, "=gc", gc_setup) != LUA_OK) {
        lua_close(L);
        return false;
    }

    daif = irq_disable();

    for (i = 0; i < GC_STEPS; i++) {
        uint64_t start = now();
        uint64_t took;

        lua_gc(L, LUA_GCSTEP, 1);

        took = now() - start;
        if (took > worst) {
            worst = took;
        }
    }

    irq_restore(daif);

    out->total = worst;
    out->iterations = 1;

    lua_close(L);
    return true;
}

static const struct benchmark benchmarks[] = {
    { "ipc_roundtrip",   bench_ipc_roundtrip },
    { "context_switch",  bench_context_switch },
    { "exception",       bench_exception },
    { "serialize",       bench_serialize },
    { "gc_pause_max",    bench_gc_pause },
};

#define BENCH_COUNT (sizeof(benchmarks) / sizeof(benchmarks[0]))

void bench_run(void)
{
    unsigned i;
    bool ok = true;

    kputs("\n# benchmarks, in counter ticks per operation\n");

    for (i = 0; i < BENCH_COUNT; i++) {
        struct bench_result r = { 0, 0 };

        if (!benchmarks[i].run(&r) || r.iterations == 0) {
            kputs("bench-fail ");
            kputs(benchmarks[i].name);
            kputc('\n');
            ok = false;
            continue;
        }

        /*
         * Reported in thousandths, so the host does not have to guess how
         * much precision was lost to integer division. A round trip costing
         * 18.4 ticks prints as 18400.
         */
        kputs("bench ");
        kputs(benchmarks[i].name);
        kputc(' ');
        kputu(r.total * 1000 / r.iterations);
        kputc(' ');
        kputu(r.iterations);
        kputc('\n');
    }

    semihosting_exit(ok ? 0 : 1);
}
