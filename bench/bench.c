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
#include "process.h"

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

        if (ipc_receive(ipc_server_cap, &msg, &sender, false) != IPC_OK) {
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
 * A typical message, packed and unpacked, and the longest the collector
 * stops for.
 *
 * `roadmap.md` asks for both at M4. The first is the cost `design.md` §1's
 * thesis is paid for with - the protocol between servers is the data model
 * of the language, and this is what that conversion costs per message, twice
 * for every namespace read from M5 and every drawing command from M6. The
 * second is what `design.md` §5.2 calls this project's recurring problem.
 *
 * Both used to open a `lua_State` inside the kernel. There is none to open
 * any more, so each is a process: `user/tests/luabench.lua` does the
 * measuring and hands the number back, and what is left here is the harness
 * that starts it and blocks until it reports.
 *
 * Blocking is the part that matters. A bounded spin waiting for the process
 * to exit would leave this thread in the run queue for the whole
 * measurement, and every timer tick would switch into it and charge its
 * work to the number being measured. A rendezvous receive takes this thread
 * out of the queue entirely, so what is being measured is the only thing
 * runnable.
 *
 * The one failure mode this shape has: a process that dies before reporting
 * leaves this thread blocked and the run hangs until the host's timeout. The
 * Lua side reports on every path it has, so reaching that means a fault -
 * which is a kernel bug and wants looking at rather than timing out politely.
 */

/* Has to match LUABENCH_BASE in user/init/main.c. */
#define LUABENCH_BASE   2000UL

static bool bench_in_a_process(unsigned long role, struct bench_result *out)
{
    extern const unsigned char init_image[];
    extern const unsigned long init_image_len;
    struct message msg;
    struct thread *sender = NULL;
    struct process *p;
    cap_t ep;
    unsigned i;

    ep = ipc_endpoint_create();
    if (ep < 0) {
        return false;
    }

    p = process_create("bench", init_image, (size_t)init_image_len,
                       LUABENCH_BASE + role);

    if (p == NULL) {
        (void)ipc_endpoint_destroy(ep);
        return false;
    }

    /* Before it runs, not after. A process that starts before its
     * capabilities are in place finds an empty table. */
    if (ipc_cap_grant(p->thread, ep) != 0) {
        process_abandon(p);
        (void)ipc_endpoint_destroy(ep);
        return false;
    }

    /* So that a failure says what it was instead of arriving as a zero. */
    process_grant_console(p);
    process_start(p);

    /*
     * Total first, then iterations, in the tag of each.
     *
     * The tag is a raw 64-bit field the kernel copies without looking at.
     * The body is a serialised Lua value and there is nothing on this side
     * that can read one - which is the whole point of the commit this
     * arrived in. Two exchanges rather than two fields, so neither side
     * holds a count the other could drift from.
     *
     * **The reply is the request, sent back unchanged.** Not a nicety: a
     * reply is unpacked into a Lua value on arrival, and an empty message is
     * not a value. Replying with a zero-length one raises inside the caller,
     * which ends the process, which leaves the next receive here waiting for
     * a sender that no longer exists - a hang rather than a failure, and the
     * first version of this function did exactly that. Echoing what came in
     * is the one reply this side can build without a lua_State.
     */
    if (ipc_receive(ep, &msg, &sender, false) != IPC_OK) {
        (void)ipc_endpoint_destroy(ep);
        return false;
    }

    out->total = msg.tag;
    (void)ipc_reply(sender, &msg);

    if (ipc_receive(ep, &msg, &sender, false) != IPC_OK) {
        (void)ipc_endpoint_destroy(ep);
        return false;
    }

    out->iterations = (unsigned long)msg.tag;
    (void)ipc_reply(sender, &msg);

    for (i = 0; i < 100000 && !p->exited; i++) {
        thread_yield();
    }

    if (p->exited) {
        process_reap(p);
    }

    (void)ipc_endpoint_destroy(ep);

    return out->total > 0 && out->iterations > 0;
}

static bool bench_serialize(struct bench_result *out)
{
    return bench_in_a_process(0, out);
}

static bool bench_gc_pause(struct bench_result *out)
{
    return bench_in_a_process(2, out);
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
