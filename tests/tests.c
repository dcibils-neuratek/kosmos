/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#include <stdbool.h>
#include <stdint.h>

#include "test.h"
#include "exit.h"
#include "fault.h"
#include "machine.h"
#include "percpu.h"
#include "spinlock.h"
#include "smp.h"
#include "console.h"
#include "trap.h"
#include "pmm.h"
#include "page.h"
#include "mmu.h"
#include "kernel.h"
#include "thread.h"
#include "sched.h"
#include "ipc.h"
#include "process.h"
#include "hal.h"
#include "boot.h"

#include <string.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

/*
 * The counter, and how fast it counts.
 *
 * `cpu_cycles` is CNTPCT_EL0 on one board and RDTSC on the other;
 * `test_counter_hz` is a register the firmware set on one and a boot-time
 * calibration on the other. Exactly the difference `tests/machine.h` and
 * the architecture headers exist to hide, and the two names are kept so the
 * timer tests below read as they always did.
 */
#define cntfrq()    test_counter_hz()
#define cntpct()    cpu_cycles()

/* Defined by the linker script. Declared as arrays so taking the address is
 * the address, with no accidental dereference. */
extern char __bss_start[];
extern char __bss_end[];
extern char __stack_bottom[];
extern char __stack_top[];

/*
 * A .bss global nobody ever assigns to. `volatile` is load-bearing: without
 * it the compiler knows a static that is never written is still zero, folds
 * the comparison to true, and the test passes without reading memory at all.
 */
static volatile uint64_t bss_canary;

static bool test_bss_zeroed(void)
{
    return bss_canary == 0;
}

static bool test_bss_bounds_aligned(void)
{
    /*
     * start.S zeroes .bss sixteen bytes at a time with `stp` and no tail.
     * If the linker script ever stops aligning both ends, the last few bytes
     * are quietly left dirty and the failure shows up somewhere else
     * entirely, months later.
     */
    return ((uintptr_t)__bss_start % 16) == 0
        && ((uintptr_t)__bss_end % 16) == 0;
}

/*
 * The kernel is privileged, on whichever board.
 *
 * `cpu_current_el` already answers this in one unit for both: AArch64 reads
 * a register that says so, and x86 reads the low two bits of CS and reports
 * ring 0 as 1, so "the kernel is level 1" is true either side. That mapping
 * is written down once, in `arch/x86_64/cpu.h`, and this test does not
 * repeat it.
 */
static bool test_running_at_the_privileged_level(void)
{
    return cpu_current_el() == 1;
}

static bool test_sp_inside_boot_stack(void)
{
    /*
     * The stack lives above __bss_end, so this failing means either that the
     * linker script let the two overlap or that start.S never set sp.
     */
    uintptr_t sp = test_stack_pointer();

    return sp > (uintptr_t)__stack_bottom
        && sp <= (uintptr_t)__stack_top;
}

/*
 * M1: the exception vector.
 *
 * An undefined instruction is used rather than a null dereference, because
 * these run before the MMU exists. With translation off, address 0 on QEMU
 * virt is flash rather than a hole, so a write there does not necessarily
 * fault. The null-dereference test arrives with the MMU, once page 0 is
 * deliberately left unmapped.
 */
static bool test_undefined_instruction_faults(void)
{
    struct fault_info f;

    FAULT_EXPECT({ TEST_UNDEFINED_INSTRUCTION(); });

    if (!fault_expect_end(&f)) {
        return false;
    }

    /* An undefined instruction reports EC 0, "unknown reason". */
    return fault_was_undefined_instruction(&f);
}

static bool test_brk_faults(void)
{
    struct fault_info f;

    FAULT_EXPECT({ TEST_BREAKPOINT(); });

    return fault_expect_end(&f) && fault_was_a_breakpoint(&f);
}

#if defined(__aarch64__)
/*
 * The two that are about *this handler* rather than about the kernel, and
 * are therefore AArch64's alone.
 *
 * Both rest on stepping ELR past the faulting instruction, which is a thing
 * only a fixed-width instruction set can do - `tests/fault.h` has the
 * argument. They keep the `fault_expect_begin` form deliberately: written
 * with `FAULT_EXPECT` the second one would pass by unwinding *around* the
 * assignment it is checking, which is a test that cannot fail and therefore
 * is not one.
 */
static bool test_elr_points_at_the_faulting_instruction(void)
{
    struct fault_info f;
    uint64_t here;

    /*
     * The label sits on the faulting instruction itself, so elr has to match
     * it exactly. This is the property the whole dump rests on: if elr is
     * off by an instruction, every fault report sends you to the wrong line.
     */
    fault_expect_begin();
    __asm__ volatile(
        "adr %0, 1f\n"
        "1: udf #0\n"
        : "=r"(here));

    return fault_expect_end(&f) && f.elr == here;
}

static bool test_execution_resumes_after_an_expected_fault(void)
{
    /*
     * The handler steps elr past the faulting instruction. If it advanced by
     * the wrong amount, control would resume in the middle of the next one
     * and this would not return at all.
     */
    volatile int reached = 0;

    fault_expect_begin();
    __asm__ volatile("udf #0");
    reached = 1;

    (void)fault_expect_end(NULL);
    return reached == 1;
}
#endif /* __aarch64__ */

/* Both in `tests/cpu.h` now, with the argument for why a store has to be
 * written in assembly - it is the same argument on both boards, and the
 * instruction is the only part that differs. */
static void store_to(uintptr_t addr, uint32_t value)
{
    test_store_to(addr, value);
}

/*
 * M1's definition of done, the half that matters most: a deliberate null
 * dereference reports a readable data abort naming the address, instead of
 * hanging.
 *
 * This replaces the unaligned-store test that stood here before the MMU
 * existed. That one worked only because every access is Device memory while
 * translation is off, and unaligned accesses to Device memory are required
 * to fault. With the MMU on, RAM is Normal memory and the same store
 * succeeds silently.
 */
static bool test_null_dereference_faults(void)
{
    struct fault_info f;

    FAULT_EXPECT({ store_to(0, 1); });

    if (!fault_expect_end(&f)) {
        return false;
    }

    return fault_was_not_mapped(&f)     /* nothing is there */
        && fault_was_a_write(&f)        /* it was a write */
        && fault_address(&f) == 0;      /* and it names the address */
}

static bool test_mmu_is_on(void)
{
    return mmu_is_enabled();
}

static bool test_stack_guard_page_is_unmapped(void)
{
    /*
     * The page below the boot stack has no translation, so an overflow is an
     * abort naming the address rather than silent corruption of .bss.
     */
    extern char __stack_guard[];
    struct fault_info f;

    FAULT_EXPECT({ store_to((uintptr_t)__stack_guard, 1); });

    return fault_expect_end(&f)
        && fault_was_not_mapped(&f)
        && fault_address(&f) == (uint64_t)(uintptr_t)__stack_guard;
}

static bool test_kernel_text_is_not_writable(void)
{
    /* W^X in the kernel: .text is mapped read-only and executable. A write
     * has to be a permission fault, not a translation fault, because the
     * page is mapped. */
    extern char __text_start[];
    struct fault_info f;

    FAULT_EXPECT({ store_to((uintptr_t)__text_start, 0); });

    return fault_expect_end(&f)
        && fault_was_not_permitted(&f)
        && fault_address(&f) == (uint64_t)(uintptr_t)__text_start;
}

static bool test_rodata_is_not_writable(void)
{
    extern char __rodata_start[];
    struct fault_info f;

    FAULT_EXPECT({ store_to((uintptr_t)__rodata_start, 0); });

    return fault_expect_end(&f)
        && fault_was_not_permitted(&f);
}

static bool test_memory_still_works_through_translation(void)
{
    /* Trivial on its face, and the thing that fails when MAIR, the shared
     * attributes or the access flag are wrong: the map looks right and every
     * access comes back as garbage or faults on first touch. */
    volatile uint64_t *p = pmm_alloc_page();
    bool ok;

    if (p == NULL) {
        return false;
    }

    p[0] = 0xa5a5a5a5a5a5a5a5ULL;
    p[511] = 0x5a5a5a5a5a5a5a5aULL;
    ok = p[0] == 0xa5a5a5a5a5a5a5a5ULL && p[511] == 0x5a5a5a5a5a5a5a5aULL;

    pmm_free_page((void *)p);
    return ok;
}

static bool test_unexpected_fault_is_not_swallowed(void)
{
    /* Arming is one-shot: the second fault in a row would reach the panic
     * path. Nothing here faults twice; this only checks that end() reports
     * false when nothing fired, so a broken test cannot pass by accident.
     *
     * Armed and disarmed with nothing between them - and deliberately *not*
     * through `FAULT_EXPECT`, whose body may not contain a `return`. A
     * mechanical conversion put one there and the compiler caught it, which
     * is the whole reason that restriction is written down. */
    jmp_buf env;

    if (setjmp(env) == 0) {
        fault_expect_unwind(env);
    }

    return fault_expect_end(NULL) == false;
}

/*
 * M3: threads and the scheduler.
 */
static volatile unsigned long trace_len;
static volatile unsigned trace[64];

static void trace_push(unsigned v)
{
    if (trace_len < 64) {
        trace[trace_len++] = v;
    }
}

static void counting_thread(void *arg)
{
    unsigned id = (unsigned)(uintptr_t)arg;
    int i;

    for (i = 0; i < 3; i++) {
        trace_push(id);
        thread_yield();
    }
}

static bool test_threads_interleave(void)
{
    /*
     * Three threads, each recording its own id three times and yielding in
     * between. Under a fair policy the trace has to be the three ids
     * repeating, not one thread running to completion and then the next: a
     * context switch that silently did nothing would produce the second.
     *
     * **Interrupts off, because this is about yielding and not about
     * preemption.** The timer runs at 250 Hz and a tick landing between two
     * yields reschedules on its own, which puts an id next to itself in the
     * trace and fails a test that is asking a different question. It is
     * rare, it is likelier on a busy host, and a gate that fails one run in
     * fifty is a gate people learn to re-run instead of read.
     *
     * `thread_yield` does not need interrupts - it calls the scheduler
     * directly - so removing them removes a variable rather than the thing
     * being measured. Same reasoning and same instruction as
     * `test_processes_have_separate_address_spaces`, which masks for the
     * same kind of reason.
     */
    unsigned i;
    bool ok;

    trace_len = 0;

    cpu_irq_disable();

    for (i = 1; i <= 3; i++) {
        if (thread_create("count", counting_thread, (void *)(uintptr_t)i) == NULL) {
            cpu_irq_enable();
            return false;
        }
    }

    /* Let them run. Each needs three turns, and this thread yields between
     * each round, so a handful of yields is more than enough. */
    for (i = 0; i < 16; i++) {
        thread_yield();
    }

    /* Every id appears exactly three times, and no id appears twice in a row,
     * which is what "interleaved" actually means. */
    ok = (trace_len == 9);

    if (ok) {
        unsigned counts[4] = { 0, 0, 0, 0 };

        for (i = 0; i < 9; i++) {
            if (trace[i] < 1 || trace[i] > 3) {
                ok = false;
                break;
            }

            counts[trace[i]]++;

            if (i > 0 && trace[i] == trace[i - 1]) {
                ok = false;
                break;
            }
        }

        ok = ok && counts[1] == 3 && counts[2] == 3 && counts[3] == 3;
    }

    /* Whatever the answer. Leaving the suite with interrupts masked would
     * make every test after this one a different test. */
    cpu_irq_enable();

    return ok;
}

static volatile bool woke;

static void blocking_thread(void *arg)
{
    (void)arg;
    thread_block();
    woke = true;
}

static bool test_block_and_wake(void)
{
    /*
     * A blocked thread must not be scheduled, and must run again once woken.
     * This is the mechanism every IPC operation is built on: a receiver with
     * nothing waiting blocks, and the sender wakes it.
     */
    struct thread *t;
    unsigned i;

    woke = false;

    t = thread_create("blocker", blocking_thread, NULL);
    if (t == NULL) {
        return false;
    }

    /* Let it reach the block. */
    for (i = 0; i < 4; i++) {
        thread_yield();
    }

    if (woke) {
        return false;       /* it ran past a block it should have stopped at */
    }

    thread_wake(t);

    for (i = 0; i < 4; i++) {
        thread_yield();
    }

    return woke;
}

static volatile uint64_t saved_x19;
static volatile double saved_d8;

static void register_thread(void *arg)
{
    (void)arg;

    /*
     * Plants a known value in a callee-saved general register, yields so a
     * switch definitely happens, and reads it back.
     *
     * **The FP half is AArch64's alone, and the reason is the ABI rather
     * than the kernel.** `d8` is callee-saved there - AAPCS64 promises the
     * low half survives a call - so a switch that forgets it corrupts a
     * value with no trace of where it happened, which is exactly the bug
     * this found. System V makes *every* XMM register caller-saved, so
     * nothing on that board promises one survives a `thread_yield`, and a
     * test asserting it would be asserting something the architecture never
     * said. What x86 does guarantee is that a *preemption* preserves them,
     * which `fp: a preemption preserves d0` checks on both boards.
     */
    uint64_t x;

    test_scratch_set(0x1234);

#if defined(__aarch64__)
    test_fp_scratch_set(2.0);
#endif

    thread_yield();
    thread_yield();

    x = test_scratch_get();

#if defined(__aarch64__)
    saved_d8 = test_fp_scratch_get();
#else
    saved_d8 = 2.0;     /* not a claim this board makes; see above */
#endif

    saved_x19 = x;
}

static bool test_context_switch_preserves_registers(void)
{
    unsigned i;

    saved_x19 = 0;
    saved_d8 = 0.0;

    if (thread_create("regs", register_thread, NULL) == NULL) {
        return false;
    }

    for (i = 0; i < 8; i++) {
        thread_yield();
    }

    return saved_x19 == 0x1234 && saved_d8 == 2.0;
}

static volatile bool finished;

static void short_thread(void *arg)
{
    (void)arg;
    finished = true;
    /* Returns, which must reach thread_exit rather than jumping into
     * whatever x30 happened to hold. */
}

static bool test_a_thread_that_returns_exits_cleanly(void)
{
    unsigned before = thread_count();
    unsigned i;

    finished = false;

    if (thread_create("short", short_thread, NULL) == NULL) {
        return false;
    }

    if (thread_count() != before + 1) {
        return false;
    }

    for (i = 0; i < 8; i++) {
        thread_yield();
    }

    /*
     * It ran, it returned rather than falling off the end into whatever x30
     * held, and its slot went back to being available. The count returning
     * to where it started is what says thread_exit ran: a thread that fell
     * through would still be counted as alive.
     */
    return finished && thread_count() == before;
}

/*
 * A second policy, to prove the seam is real.
 *
 * Strict LIFO: the most recently enqueued thread runs next. It is a terrible
 * scheduler and that is the point. If installing it visibly changes the
 * order threads run in, then the policy really is separable from the
 * mechanism, which is a claim no amount of interface design proves on its
 * own.
 */
/*
 * One stack, not one per processor, and that is the point of it.
 *
 * The real policies keep a queue per core because a thread has a home and
 * does not migrate. This one deliberately ignores the `cpu` it is handed and
 * keeps everything in one place - which is legal, and is the strongest form
 * of the claim the vtable makes: a policy decides *everything* about where
 * threads go, including whether the machine has more than one queue at all.
 *
 * It only ever runs with one scheduling core, which is what makes ignoring
 * the parameter safe rather than a bug waiting to happen.
 */
static struct thread *lifo_top;

static void lifo_init(void)   { lifo_top = NULL; }

static void lifo_enqueue(unsigned cpu, struct thread *t)
{
    (void)cpu;
    t->sched.next = lifo_top;
    lifo_top = t;
}

static struct thread *lifo_pick_next(unsigned cpu)
{
    struct thread *t = lifo_top;

    (void)cpu;

    if (t == NULL) {
        return NULL;
    }

    lifo_top = t->sched.next;
    t->sched.next = NULL;
    return t;
}

static bool lifo_tick(struct thread *running) { (void)running; return false; }

static const struct scheduler sched_lifo = {
    .name      = "test-lifo",
    .init      = lifo_init,
    .enqueue   = lifo_enqueue,
    .pick_next = lifo_pick_next,
    .tick      = lifo_tick,
};

static void order_thread(void *arg)
{
    trace_push((unsigned)(uintptr_t)arg);
}

static bool run_three_and_record(const struct scheduler *policy)
{
    unsigned      i;
    unsigned long guard;

    /* Swapping policies is only safe with nothing queued, which is why this
     * runs each set of threads to completion before the next. */
    sched_use(policy);
    trace_len = 0;

    for (i = 1; i <= 3; i++) {
        if (thread_create("order", order_thread, (void *)(uintptr_t)i) == NULL) {
            return false;
        }
    }

    /*
     * Waited out by the clock and not by a count of yields.
     *
     * This was twelve yields, which sounds generous and is not a duration:
     * a yield returns as soon as something ran, so twelve of them are
     * twelve *switches* and not twelve turns for these three threads. Under
     * a loaded host that is sometimes not enough, and the test failed for
     * having been in a hurry - twice in one day, once on each board, which
     * is what finally made somebody read it.
     *
     * The same mistake `ipc: a receive with a deadline gives up` records
     * further down and for the same reason. Bounding by the clock also
     * finishes the moment the three are done, so the common case is
     * quicker rather than slower.
     */
    /*
     * **A second, and it was two hundred milliseconds.**
     *
     * Bounding by the clock rather than by a count of yields was the right
     * fix and is not the whole of it: a tick is wall-clock time, and how
     * much *work* fits inside two hundred milliseconds of it changed when
     * the suite started booting `-smp 4`. Four vCPUs under TCG round-robin
     * in one host thread, so core zero now gets roughly a quarter of the
     * host's attention and executes roughly a quarter of the instructions
     * per tick that it used to.
     *
     * The bound is a *failure* bound: it is reached only when the three
     * threads never finish, and the loop leaves the moment they do. Making
     * it five times longer costs nothing in the passing case and stops the
     * suite reporting a scheduler bug when what it measured was the host.
     */
    guard = hal_ticks() + 250;              /* a second, on a busy host */

    while (trace_len < 3 && hal_ticks() < guard) {
        thread_yield();
    }

    return trace_len == 3;
}

static bool test_the_scheduler_is_pluggable(void)
{
    unsigned fifo[3];
    unsigned lifo[3];
    unsigned i;

    if (!run_three_and_record(&sched_round_robin)) {
        return false;
    }
    for (i = 0; i < 3; i++) { fifo[i] = trace[i]; }

    if (!run_three_and_record(&sched_lifo)) {
        return false;
    }
    for (i = 0; i < 3; i++) { lifo[i] = trace[i]; }

    /*
     * Put the real policy back before anything else runs - and the real one
     * is `sched_priority`, not round robin.
     *
     * This said `sched_round_robin` because that was the default when it was
     * written, and it kept saying it after the default changed. Every test
     * after this one then ran under round robin: the two scheduler tests
     * below saw FIFO ordering and no preemption on wake, and reported the
     * priority policy broken when what was broken was this line.
     *
     * A test that changes global state and restores it by *name* rather than
     * by what it found is a test that silently disables a feature for
     * everything after it.
     */
    sched_use(&sched_priority);

    /* Created 1, 2, 3 in that order. FIFO runs them in it; LIFO reverses it.
     * Asserting the exact orders rather than merely "they differ" means a
     * policy that is broken in both directions cannot pass. */
    return fifo[0] == 1 && fifo[1] == 2 && fifo[2] == 3
        && lifo[0] == 3 && lifo[1] == 2 && lifo[2] == 1;
}

static bool test_thread_stacks_have_guard_pages(void)
{
    /*
     * The page below a thread's stack must have no translation, or an
     * overflow quietly writes into whatever the page allocator handed out
     * before it, which is another thread's stack about as often as not.
     *
     * Most of RAM is mapped in 2 MB blocks, so this only holds because
     * mmu_unmap_page splits a block when it has to. Before that existed,
     * mmu_page_entry returned NULL here and there was no guard at all.
     *
     * Checked on a created thread rather than the current one: thread 0 is
     * adopted from the boot code and uses the linker's stacks, whose guards
     * come from the linker script instead.
     */
    struct thread *t = thread_create("guard", short_thread, NULL);
    uint64_t *stack_guard;
    uint64_t *exception_guard;
    unsigned i;

    if (t == NULL) {
        return false;
    }

    /* The lowest page of each allocation is the guard. */
    stack_guard     = mmu_page_entry((uintptr_t)t->stack);
    exception_guard = mmu_page_entry((uintptr_t)t->exception_stack);

    if (stack_guard == NULL || exception_guard == NULL) {
        return false;   /* still described by a block: the split did not happen */
    }

    if ((*stack_guard & 1) != 0 || (*exception_guard & 1) != 0) {
        return false;   /* mapped, so not a guard */
    }

    /* Let it run and exit rather than leaving it queued for the next test. */
    for (i = 0; i < 8; i++) {
        thread_yield();
    }

    return true;
}

/*
 * M4: processes at EL0.
 */
extern char user_hello_start[],        user_hello_end[];
extern char user_fault_null_start[],   user_fault_null_end[];
extern char user_fault_kernel_start[], user_fault_kernel_end[];
extern char user_fault_text_start[],   user_fault_text_end[];
extern char user_bad_pointer_start[],  user_bad_pointer_end[];

/* Runs a blob to completion and returns its exit code, or a large negative
 * on anything going wrong. The process is reaped, so the slot comes back. */
static int run_user(const char *name, const char *start, const char *end)
{
    struct process *p = process_create(name, start, (size_t)(end - start), 0);
    unsigned i;
    int code;

    if (p == NULL) {
        return -1000;
    }

    /*
     * These fixtures report by writing to the console, so they are given it.
     * The console gate is what the shell runs into, and it is exactly the
     * point: a process that was not handed the device cannot reach it. Here
     * the device *is* how the fixture answers, so it gets one.
     */
    process_grant_console(p);

    process_start(p);

    /* Bounded, so a process that never exits fails the test rather than
     * hanging the suite. It is preempted, so yielding is not required for
     * it to make progress; this just waits. */
    for (i = 0; i < 200 && !p->exited; i++) {
        thread_yield();
    }

    if (!p->exited) {
        return -1001;
    }

    code = p->exit_code;
    process_reap(p);
    return code;
}

static bool test_lua_runs_at_el0(void)
{
    /*
     * The init image: a real link of Lua, the libc and libm at a user
     * address, carried inside the kernel image and copied into a process.
     *
     * Run in the role that needs no capabilities, because what is being
     * checked is that the language works out here at all: arithmetic and
     * floats, math.sqrt out of newlib's libm, coroutines, an error raised
     * inside the VM and caught by pcall, the absence of io, os and debug, and
     * the collector reclaiming memory on a heap it cannot grow. Any of them
     * failing raises, which makes the chunk return non-zero.
     *
     * Its output goes to the console rather than into an assertion, because
     * what is being asserted is that all of it ran at EL0 and came back with
     * a zero.
     */
    extern const unsigned char init_image[];
    extern const unsigned long init_image_len;
    unsigned before = process_count();

    struct process *p = process_create("t-init", init_image,
                                       (size_t)init_image_len, 3 /* selftest */);
    unsigned i;
    int code;

    if (p == NULL) {
        kputs("\n   (no room to create the process)\n");
        return false;
    }

    process_start(p);

    for (i = 0; i < 4096 && !p->exited; i++) {
        thread_yield();
    }

    if (!p->exited) {
        /* A test that fails should say which way it failed. This one has
         * three, and telling them apart used to mean reading the source
         * and guessing. */
        kputs("\n   (still running after 4096 yields)\n");
        return false;
    }

    code = p->exit_code;

    if (code != 0) {
        kputs("\n   (the chunk exited with ");
        kputu((unsigned long)code);
        kputs(")\n");
    }
    process_reap(p);

    if (process_count() != before) {
        kputs("\n   (processes: ");
        kputu(before);
        kputs(" before, ");
        kputu(process_count());
        kputs(" after)\n");
    }

    return code == 0 && process_count() == before;
}

static bool test_two_processes_exchange_a_lua_table(void)
{
    /*
     * M4's definition of done, in one test.
     *
     * Two processes at EL0, in separate address spaces, exchanging a Lua
     * table over the microkernel's IPC. The same image runs as both; the
     * kernel tells each which it is and hands each a capability for one
     * endpoint, and neither can name anything else.
     *
     * The client checks what came back and exits non-zero if any of it is
     * wrong: strings, integers that stayed integers, floats that stayed
     * floats, a nested table, and a list. It also checks that a function and
     * a cyclic table are refused rather than mangled.
     */
    extern const unsigned char init_image[];
    extern const unsigned long init_image_len;
    struct process *server;
    struct process *client;
    cap_t ep;
    unsigned i;
    int code;

    ep = ipc_endpoint_create();
    if (ep < 0) {
        return false;
    }

    server = process_create("t-echo", init_image, (size_t)init_image_len, 1);
    client = process_create("t-cli", init_image, (size_t)init_image_len, 0);

    if (server == NULL || client == NULL) {
        (void)ipc_endpoint_destroy(ep);
        return false;
    }

    /* Granted before either can run. Capability zero by convention, because
     * a fresh process's table is empty and this is the first thing in it. */
    if (ipc_cap_grant(server->thread, ep) != 0
        || ipc_cap_grant(client->thread, ep) != 0) {
        (void)ipc_endpoint_destroy(ep);
        return false;
    }

    process_start(server);
    process_start(client);

    for (i = 0; i < 8192 && !client->exited; i++) {
        thread_yield();
    }

    if (!client->exited) {
        (void)ipc_endpoint_destroy(ep);
        return false;
    }

    code = client->exit_code;
    process_reap(client);

    /* Destroying the endpoint is what tells the server to stop: its receive
     * fails and it leaves its loop. The same wake-the-blocked path M3 built,
     * now reaching across an address space. */
    (void)ipc_endpoint_destroy(ep);

    for (i = 0; i < 512 && !server->exited; i++) {
        thread_yield();
    }

    if (server->exited) {
        process_reap(server);
    }

    return code == 0;
}

static bool test_a_process_can_spawn_and_wait(void)
{
    /*
     * init in userland needs two things: a process able to start processes,
     * and one able to notice when they end. This checks both, and the one
     * property that makes them safe.
     *
     * A parent cannot promote a child beyond itself. Every capability in a
     * spawn is resolved against the parent's own table, and passing on the
     * console is refused unless the parent holds it - otherwise any process
     * could promote itself by spawning a child and asking it to print.
     *
     * The test process is deliberately *not* given the console, so its own
     * report goes nowhere; what it says is its exit code.
     */
    extern const unsigned char init_image[];
    extern const unsigned long init_image_len;
    struct process *p;
    unsigned i;
    int code;

    p = process_create("t-spawn", init_image,
                       (size_t)init_image_len, 8 /* spawntest */);
    if (p == NULL) {
        return false;
    }

    process_start(p);

    for (i = 0; i < 16384 && !p->exited; i++) {
        thread_yield();
    }

    if (!p->exited) {
        return false;
    }

    code = p->exit_code;
    process_reap(p);

    /*
     * Distinct codes per check, because this fixture cannot print: it
     * deliberately does not hold the console, since the last thing it checks
     * is that it cannot get one. 10 spawn failed, 11 wrong child, 12 the
     * child failed, 13 wait invented a child, 14 it handed out a console it
     * does not hold.
     */
    return code == 0;
}

static bool test_only_the_console_owner_may_print(void)
{
    /*
     * What makes the console server a server rather than a convention.
     *
     * The same fixture, run twice: once holding the console and once not.
     * With it, the write reports the byte count it wrote. Without it, the
     * syscall refuses before it looks at anything else, and the process
     * exits with the code it uses to say so.
     *
     * If any process could print, nothing would depend on going through the
     * console server, and the design would hold by agreement rather than
     * because the machine says so. This is the difference, measured.
     */
    extern const unsigned char init_image[];    /* unused; keeps the shape */
    struct process *p;
    unsigned i;
    int with_console;
    int without;

    (void)init_image;

    /* With the console. `hello` exits 0 only if its write returned the
     * length it asked for. */
    p = process_create("t-con-y", user_hello_start,
                       (size_t)(user_hello_end - user_hello_start), 0);
    if (p == NULL) {
        return false;
    }
    process_grant_console(p);
    process_start(p);

    for (i = 0; i < 200 && !p->exited; i++) {
        thread_yield();
    }
    if (!p->exited) {
        return false;
    }
    with_console = p->exit_code;
    process_reap(p);

    /* Without it. The same code, refused. */
    p = process_create("t-con-n", user_hello_start,
                       (size_t)(user_hello_end - user_hello_start), 0);
    if (p == NULL) {
        return false;
    }
    process_start(p);

    for (i = 0; i < 200 && !p->exited; i++) {
        thread_yield();
    }
    if (!p->exited) {
        return false;
    }
    without = p->exit_code;
    process_reap(p);

    /* 1 is what hello exits with when the write did not return what it
     * asked for, which is what being refused looks like from inside. */
    return with_console == 0 && without == 1;
}

static bool test_the_same_server_under_two_names(void)
{
    /*
     * The first half of M5's definition of done: mount the same server at two
     * different paths in two different processes, and have each see only its
     * own.
     *
     * Three processes and one endpoint. The ramfs serves; the two clients
     * mount it under names of their own choosing and each checks that the
     * other's name does not exist in its world. The clients assert as well as
     * print, so a broken run comes back as a non-zero exit rather than as
     * output nobody reads.
     *
     * `design.md` §2 is precise about the answer being "no such path" and not
     * "permission denied", and that falls out of the mechanism rather than
     * being arranged: resolution is a lookup, so an unmounted prefix matches
     * nothing and there is nothing to deny.
     */
    extern const unsigned char init_image[];
    extern const unsigned long init_image_len;
    size_t len = (size_t)init_image_len;
    struct process *ramfs;
    struct process *a;
    struct process *b;
    cap_t ep;
    unsigned i;
    bool ok;

    ep = ipc_endpoint_create();
    if (ep < 0) {
        return false;
    }

    ramfs = process_create("t-ramfs", init_image, len, 1);
    a     = process_create("t-cli-a", init_image, len, 0);
    b     = process_create("t-cli-b", init_image, len, 2);

    if (ramfs == NULL || a == NULL || b == NULL) {
        (void)ipc_endpoint_destroy(ep);
        return false;
    }

    if (ipc_cap_grant(ramfs->thread, ep) != 0
        || ipc_cap_grant(a->thread, ep) != 0
        || ipc_cap_grant(b->thread, ep) != 0) {
        (void)ipc_endpoint_destroy(ep);
        return false;
    }

    process_start(ramfs);
    process_start(a);
    process_start(b);

    for (i = 0; i < 16384 && !(a->exited && b->exited); i++) {
        thread_yield();
    }

    ok = a->exited && b->exited && a->exit_code == 0 && b->exit_code == 0;

    process_reap(a);
    process_reap(b);

    /* And stopping the server is the M3 trap, now reaching across an address
     * space: destroying the endpoint fails its receive and it leaves its
     * loop rather than waiting forever. */
    (void)ipc_endpoint_destroy(ep);

    for (i = 0; i < 1024 && !ramfs->exited; i++) {
        thread_yield();
    }

    ok = ok && ramfs->exited;

    if (ramfs->exited) {
        process_reap(ramfs);
    }

    return ok;
}

static bool test_a_process_runs_at_el0(void)
{
    /*
     * It writes through a syscall, checks the byte count that came back, and
     * exits 0 only if it matched. A process that never reached EL0, or whose
     * syscall return value was dropped on the way home, exits non-zero.
     */
    return run_user("t-hello", user_hello_start, user_hello_end) == 0;
}

static bool test_a_null_dereference_kills_only_the_process(void)
{
    /*
     * M4's definition of done. The process reads address zero, which has no
     * translation in its address space any more than it does in the
     * kernel's, and dies. This test running at all afterwards is the other
     * half of the assertion.
     */
    unsigned before = process_count();
    int code = run_user("t-null", user_fault_null_start, user_fault_null_end);

    /* -1 is what the fault path exits with. 41 is what the program exits
     * with if the fault never happened, which would mean address zero was
     * readable. */
    return code == -1 && process_count() == before;
}

static bool test_a_process_cannot_read_the_kernel(void)
{
    /*
     * The isolation, stated as plainly as it can be. The address it reads is
     * the kernel image, which is mapped in this process's own page tables,
     * because there is no TTBR1 split and every space contains the kernel.
     * It still cannot be touched, because those mappings are AP=00: EL1
     * read/write, EL0 nothing.
     *
     * That is why the kernel living in TTBR0 is a layout decision and not a
     * security one.
     */
    unsigned before = process_count();
    int code = run_user("t-kern", user_fault_kernel_start,
                        user_fault_kernel_end);

    return code == -1 && process_count() == before;
}

static bool test_a_process_cannot_write_its_own_code(void)
{
    /* Mapped EL0 read-only and executable. A process able to rewrite its
     * own text could defeat anything checked about it beforehand. */
    unsigned before = process_count();
    int code = run_user("t-text", user_fault_text_start, user_fault_text_end);

    return code == -1 && process_count() == before;
}

static bool test_a_syscall_refuses_a_kernel_pointer(void)
{
    /*
     * The one that must survive, and the one the hardware does not help
     * with. The kernel would dereference that pointer at EL1, where the
     * mapping is valid and privileged; nothing faults. Only the explicit
     * check in the syscall path refuses it.
     *
     * Being told no is not the same as being killed: the process exits 0,
     * having seen SYS_ERR_FAULT come back.
     */
    return run_user("t-badptr", user_bad_pointer_start,
                    user_bad_pointer_end) == 0;
}

static bool test_processes_have_separate_address_spaces(void)
{
    /*
     * Two processes, and what they do and do not share.
     *
     * **This used to check that their code was a different physical page,
     * and that is now deliberately false.** The read-only half of an image
     * is mapped where it lies - one copy for every process that runs it -
     * because a page nobody can write is a page nobody can use to reach
     * anybody. So the check is the invariant rather than its old proxy:
     * the code page is *the same* and is read-only to EL0, and everything
     * writable is different.
     *
     * Asserting the sharing rather than tolerating it is the point. A test
     * that merely stopped looking would pass just as well if the sharing
     * quietly stopped happening, and the 2.8 MB a process it saves would go
     * back without anything noticing.
     *
     * Checked from the kernel by walking each space rather than from inside
     * them, since neither can see the other by construction, which is what
     * is being tested.
     */
    struct process *a;
    struct process *b;
    uint64_t *ea;
    uint64_t *eb;
    bool ok;
    unsigned i;

    /*
     * Interrupts off for the inspection.
     *
     * Both processes are runnable the moment they are created, and
     * preemption means either can run and exit before the next line here.
     * process_exit frees the address space and NULLs the pointer, so reading
     * `space` without masking is reading a pointer that is entitled to have
     * been freed - which is exactly what it did, faulting on a NULL address
     * space about one run in five.
     *
     * This is the "where a lock will go" case, standing in for a lock that
     * does not exist yet. See the comment on struct process.
     */
    cpu_irq_disable();

    a = process_create("t-sp-a", user_hello_start,
                       (size_t)(user_hello_end - user_hello_start), 0);
    b = process_create("t-sp-b", user_hello_start,
                       (size_t)(user_hello_end - user_hello_start), 0);

    if (a == NULL || b == NULL) {
        cpu_irq_enable();
        return false;
    }

    ea = as_page_entry(a->space, USER_TEXT_VA);
    eb = as_page_entry(b->space, USER_TEXT_VA);

    ok = ea != NULL && eb != NULL
      && test_pte_is_mapped(*ea) && test_pte_is_mapped(*eb)
      /* The same page, on purpose. */
      && test_pte_frame(*ea) == test_pte_frame(*eb)
      /*
       * And read-only to the process in both, which is the whole reason
       * sharing it is safe: were it writable the two processes would be
       * writing each other's code.
       */
      && test_pte_is_read_only_to_a_process(*ea)
      && test_pte_is_read_only_to_a_process(*eb);

    /* Their stacks and their heaps are different pages, which is the claim
     * the code page used to stand in for: writable memory is never shared,
     * and nothing is shared by accident rather than only the thing that was
     * checked first. */
    {
        uintptr_t stack_page = USER_STACK_TOP - PAGE_SIZE;
        uint64_t *sa = as_page_entry(a->space, stack_page);
        uint64_t *sb = as_page_entry(b->space, stack_page);
        uint64_t *ha = as_page_entry(a->space, USER_HEAP_VA);
        uint64_t *hb = as_page_entry(b->space, USER_HEAP_VA);

        ok = ok && sa != NULL && sb != NULL
                && test_pte_is_mapped(*sa) && test_pte_is_mapped(*sb)
                && test_pte_frame(*sa) != test_pte_frame(*sb);

        ok = ok && ha != NULL && hb != NULL
                && test_pte_is_mapped(*ha) && test_pte_is_mapped(*hb)
                && test_pte_frame(*ha) != test_pte_frame(*hb);
    }

    /*
     * And the page above the stack has no translation, so an overflow
     * faults. as_page_entry returns where the descriptor *would* be, which
     * is non-NULL as soon as the table exists, so the question to ask is
     * whether the descriptor is valid rather than whether the pointer is.
     */
    {
        uint64_t *above = as_page_entry(a->space, USER_STACK_TOP);
        ok = ok && (above == NULL || (*above & 1) == 0);
    }

    /*
     * Everything that had to be read before they could run has been read, so
     * they can run now. Starting them explicitly is also why the masking
     * above is belt and braces rather than the only thing holding this
     * together.
     */
    process_start(a);
    process_start(b);

    cpu_irq_enable();

    for (i = 0; i < 200 && (!a->exited || !b->exited); i++) {
        thread_yield();
    }

    ok = ok && a->exited && b->exited;
    process_reap(a);
    process_reap(b);
    return ok;
}

/*
 * M3: address spaces.
 */
extern char __text_start[];

static bool test_a_new_space_contains_the_kernel(void)
{
    /*
     * There is no TTBR1 split yet, so the kernel is identity mapped through
     * TTBR0 like everything else. A space that did not contain it would
     * fault on the instruction after the switch, and the handler would have
     * no translation either. This checks the copy happened before anything
     * relies on it.
     */
    struct addrspace *as = as_create();
    uint64_t *there;
    uint64_t *here;
    bool ok;

    if (as == NULL) {
        return false;
    }

    there = as_page_entry(as, (uintptr_t)__text_start);
    here  = mmu_page_entry((uintptr_t)__text_start);

    /* The same descriptor, because the tables below the top level are
     * shared rather than copied. */
    ok = there != NULL && here != NULL && *there == *here;

    as_destroy(as);
    return ok;
}

static bool test_a_space_maps_and_unmaps(void)
{
    struct addrspace *as = as_create();
    void *page;
    uint64_t *entry;
    bool ok;

    if (as == NULL) {
        return false;
    }

    page = pmm_alloc_page();
    if (page == NULL) {
        as_destroy(as);
        return false;
    }

    ok = as_map(as, USER_VA_BASE, (uintptr_t)page, 1, MAP_RW) == AS_OK;

    entry = as_page_entry(as, USER_VA_BASE);
    ok = ok && entry != NULL
            && test_pte_is_mapped(*entry)
            && test_pte_frame(*entry) == (uintptr_t)page;

    /* And the kernel's own map is untouched: this is a different space. */
    ok = ok && mmu_page_entry(USER_VA_BASE) == NULL;

    ok = ok && as_unmap(as, USER_VA_BASE, 1) == AS_OK;
    entry = as_page_entry(as, USER_VA_BASE);
    ok = ok && entry != NULL && !test_pte_is_mapped(*entry);

    pmm_free_page(page);
    as_destroy(as);
    return ok;
}

static bool test_a_space_refuses_the_kernel_region(void)
{
    /*
     * The check that makes the sharing safe. Below USER_VA_BASE a space
     * resolves through tables it shares with the kernel, so a mapping there
     * would appear in the kernel's own map and in every other space, and it
     * would look like it had worked.
     */
    struct addrspace *as = as_create();
    bool ok;

    if (as == NULL) {
        return false;
    }

    /*
     * The addresses are derived from `USER_VA_BASE` rather than written
     * out, and that is the whole of what this test learned from a second
     * board.
     *
     * It used to name 0x40000000 and 0x09000000 - a kernel address and a
     * device address, on ARM. On x86-64 `USER_VA_BASE` *is* 0x40000000, so
     * the first line asked the kernel to refuse the first page of user
     * space, the kernel quite correctly mapped it, and the test reported a
     * failure that was entirely its own. A literal address in a portable
     * test is a claim about one machine.
     *
     * The physical page is real because the range check has to be reached
     * with something valid behind it; which page it is does not matter,
     * since none of these four calls is meant to get as far as using it.
     */
    void *page = pmm_alloc_page();

    if (page == NULL) {
        as_destroy(as);
        return false;
    }

    ok = /* One page below where user space starts. */
         as_map(as, USER_VA_BASE - PAGE_SIZE, (uintptr_t)page, 1, MAP_RW)
             == AS_ERR_RANGE
         /* And a low address, well inside the half the kernel keeps. */
      && as_map(as, PAGE_SIZE, (uintptr_t)page, 1, MAP_RW) == AS_ERR_RANGE
         /* Not page-aligned, which is a different refusal. */
      && as_map(as, USER_VA_BASE + 1, (uintptr_t)page, 1, MAP_RW)
             == AS_ERR_ALIGN
         /* And one past the end. */
      && as_map(as, USER_VA_END, (uintptr_t)page, 1, MAP_RW) == AS_ERR_RANGE;

    pmm_free_page(page);
    as_destroy(as);
    return ok;
}

static bool test_a_space_refuses_a_page_count_that_wraps(void)
{
    /*
     * A page count large enough to overflow `pages * PAGE_SIZE`.
     *
     * 2^52 + 1 pages multiplies to 4096 on a 64-bit size_t, so the request
     * arrives at every bound check looking like a single page: it starts
     * inside the user window, it ends one page later, and `end > va` - the
     * only overflow guard there was - is perfectly happy, because that
     * catches a wrap to exactly zero and nothing else.
     *
     * What it is not is one page. `as_unmap` loops `pages` times, so the
     * accepted request walks out of the user window and off the end of the
     * address space, four and a half quadrillion iterations later. Any
     * process could ask for it.
     *
     * The fix is to bound the count before multiplying it, and this is the
     * test that says so. It has to be a *count* test rather than an address
     * one: every address in the request is legal.
     */
    struct addrspace *as = as_create();
    size_t wraps = ((size_t)1 << 52) + 1;
    bool ok;

    if (as == NULL) {
        return false;
    }

    ok = as_unmap(as, USER_VA_BASE, wraps) == AS_ERR_RANGE
      && as_map(as, USER_VA_BASE, 0x40000000UL, wraps, MAP_RW) == AS_ERR_RANGE;

    /* And the wrap to exactly zero, which was already refused and must
     * stay refused now that the count is checked first. */
    ok = ok && as_unmap(as, USER_VA_BASE, (size_t)1 << 52) == AS_ERR_RANGE;

    as_destroy(as);
    return ok;
}

static bool test_switching_to_a_space_makes_its_mapping_real(void)
{
    /*
     * The test that proves the rest is not bookkeeping: switch to the space,
     * write through the mapped address, switch back, and read the value
     * through the identity map. Same physical page, two virtual addresses,
     * and only one of them exists at a time.
     */
    struct addrspace *as = as_create();
    volatile uint64_t *physical;
    volatile uint64_t *mapped = (volatile uint64_t *)USER_VA_BASE;
    void *page;
    struct fault_info f;
    bool ok;

    if (as == NULL) {
        return false;
    }

    page = pmm_alloc_page();
    if (page == NULL) {
        as_destroy(as);
        return false;
    }

    physical = page;
    *physical = 0;

    if (as_map(as, USER_VA_BASE, (uintptr_t)page, 1, MAP_RW) != AS_OK) {
        pmm_free_page(page);
        as_destroy(as);
        return false;
    }

    as_switch(as);
    *mapped = 0xfeedfacecafebeefULL;
    as_switch(NULL);

    ok = *physical == 0xfeedfacecafebeefULL;

    /* And back in the kernel's space that address has no translation. */
    FAULT_EXPECT({ store_to(USER_VA_BASE, 1); });
    ok = ok && fault_expect_end(&f)
            && fault_was_not_mapped(&f)
            && fault_address(&f) == USER_VA_BASE;

    pmm_free_page(page);
    as_destroy(as);
    return ok;
}

static bool test_destroying_a_space_returns_its_pages(void)
{
    /*
     * A space that leaked its tables would be invisible until something ran
     * out of memory a long way away. Mapping across two level 2 boundaries
     * makes it allocate several tables rather than one.
     */
    size_t before = pmm_free_pages();
    struct addrspace *as = as_create();

    if (as == NULL) {
        return false;
    }

    if (as_map(as, USER_VA_BASE, 0x40000000UL, 1, MAP_RW) != AS_OK
        || as_map(as, USER_VA_BASE + 0x200000UL, 0x40000000UL, 1, MAP_RW) != AS_OK
        || as_map(as, USER_VA_BASE + 0x40000000UL, 0x40000000UL, 1, MAP_RW) != AS_OK) {
        as_destroy(as);
        return false;
    }

    if (pmm_free_pages() >= before) {
        /*
         * **Destroyed before returning, and it was not.**
         *
         * This path returns the verdict without giving the space back, so a
         * failure here took one of the thirty-two address-space slots out
         * of circulation permanently - and the next test to want all of
         * them, `as: one space per possible process`, then failed for a
         * reason that had nothing to do with it. A test that leaks on its
         * failure path turns one red line into two, and the second one is
         * the one people go and look at.
         */
        as_destroy(as);
        return false;   /* it allocated nothing, so there is nothing to test */
    }

    as_destroy(as);

    return pmm_free_pages() == before;
}

/*
 * M3: preemption.
 */

/* `cntfrq` and `cntpct` are defined with the timer tests further down and
 * are used here to bound a wait by elapsed time rather than by a loop
 * count, so a kernel without preemption fails quickly instead of hanging.
 * They are macros over `tests/machine.h` now and need no declaration. */

static volatile bool          spinner_stop;
static volatile unsigned long spinner_laps;

/*
 * A second lap counter, so two spinners on one processor can be told apart.
 *
 * Both share `spinner_stop`, which is right: one flag stops them together
 * and there is nothing to be learned from stopping them separately.
 */
static volatile unsigned long spinner2_laps;

static void spinner(void *arg)
{
    (void)arg;

    /*
     * Never yields, never blocks, never sleeps. Before preemption existed
     * this thread would own the machine from the moment it was scheduled.
     */
    while (!spinner_stop) {
        spinner_laps++;
    }
}

/*
 * The same loop against the other counter. A separate function rather than
 * an argument because the thread entry signature takes a `void *` and a
 * pointer to a `volatile unsigned long` would be the only place in this
 * file that does that - two lines of duplication against one of indirection.
 */
static void spinner2(void *arg)
{
    (void)arg;

    while (!spinner_stop) {
        spinner2_laps++;
    }
}

static bool test_a_thread_that_never_yields_is_preempted(void)
{
    /*
     * The point of preemption, and the only test that can distinguish it
     * from cooperative scheduling: neither thread here yields.
     *
     * The main thread waits on the counter rather than on the scheduler, so
     * the only way the spinner can run at all is if the timer takes the CPU
     * away from this one. The wait is bounded by the counter so a kernel
     * without preemption fails in a tenth of a second instead of hanging.
     */
    uint64_t deadline;
    unsigned long before;
    unsigned i;
    bool ran;

    spinner_stop = false;
    spinner_laps = 0;

    if (thread_create("spinner", spinner, NULL) == NULL) {
        return false;
    }

    before = spinner_laps;
    deadline = cntpct() + cntfrq() / 2;      /* half a second */

    while (cntpct() < deadline && spinner_laps == before) {
        /* Deliberately empty. Calling thread_yield here would hand the CPU
         * over voluntarily and prove nothing. */
    }

    ran = spinner_laps > before;

    spinner_stop = true;

    /* Let it see the flag and exit. It is preempted back in on its own. */
    deadline = cntpct() + cntfrq() / 2;
    while (cntpct() < deadline) {
        /* nothing */
    }

    for (i = 0; i < 8; i++) {
        thread_yield();
    }

    return ran;
}

/*
 * Two threads on *another* processor, neither of which yields.
 *
 * **The regression this exists for went unnoticed for three releases and
 * was invisible to every other check here.** `thread_tick` returned before
 * `policy->tick` on every core but zero, so a secondary never preempted on
 * a quantum: whatever it was running ran until it blocked or exited. The
 * suite could not see it because the suite pins itself to core zero with
 * `thread_place_across(1)`, and core zero was the one core that worked.
 *
 * The shape is what makes it a test of preemption rather than of placement.
 * Two spinners are put on the same secondary and neither ever yields, so
 * the *only* way the second one can run a single lap is if the timer takes
 * the processor away from the first. Waiting on the counter rather than on
 * the scheduler keeps this thread out of the answer.
 *
 * `thread_create_on` rather than a placement switch, for the reason the
 * rest of the SMP checks use it: the question is about one named
 * processor, and a global policy would make it a question about whichever
 * core the round robin happened to reach.
 */
static bool test_a_secondary_preempts_a_thread_that_never_yields(void)
{
    uint64_t deadline;
    unsigned long first_before, second_before;
    unsigned i;
    bool both_ran;

    /* A machine with one processor has nothing to ask. */
    if (smp_online() < 2) {
        return true;
    }

    spinner_stop = false;
    spinner_laps = 0;
    spinner2_laps = 0;

    if (thread_create_on(1, "spin-a", spinner, NULL) == NULL) {
        return false;
    }

    if (thread_create_on(1, "spin-b", spinner2, NULL) == NULL) {
        spinner_stop = true;
        return false;
    }

    first_before  = spinner_laps;
    second_before = spinner2_laps;

    /*
     * A second, and it is a *failure* bound: the loop leaves as soon as
     * both have run, so the passing case costs whatever it costs. Long
     * because four vCPUs share one host thread under TCG, so a tick here
     * buys a quarter of the instructions it would on the real machine.
     */
    deadline = cntpct() + cntfrq();

    while (cntpct() < deadline
           && !(spinner_laps > first_before && spinner2_laps > second_before)) {
        /* Deliberately empty, and this thread is on core zero: yielding
         * here would prove something about this processor instead. */
    }

    both_ran = spinner_laps > first_before && spinner2_laps > second_before;

    spinner_stop = true;

    /* Let them see the flag and leave. They are preempted back in to do
     * it - which is the same mechanism, one more time. */
    deadline = cntpct() + cntfrq() / 2;
    while (cntpct() < deadline) {
        /* nothing */
    }

    for (i = 0; i < 8; i++) {
        thread_yield();
    }

    return both_ran;
}

/*
 * A new thread goes where there is room, not where the counter points.
 *
 * **Placement used to be `next_cpu % cores` - round robin over creation
 * events - and that deals out *creations* rather than work.** It counts
 * every thread the machine has ever made, so where the next one lands
 * depends on how many have been made before it and not at all on what any
 * processor is doing. Two threads that end together free their cores and
 * the next two land somewhere else entirely.
 *
 * Measured, with four workers on four cores, killing the two on the highest
 * cores and starting two more:
 *
 *     round robin   per-core 2 2 0 0     both onto the busy pair
 *     least loaded  per-core 1 1 1 1     onto the two that were freed
 *
 * And on the desktop it was one core at 91% with two under 3%, because two
 * 3D demos and the window manager had been dealt onto the same processor.
 *
 * The check fills every core but the last by hand, then asks for one more
 * and requires it to land on the empty one. It is deterministic under the
 * policy - an empty core is strictly emptier than the rest, whatever the
 * rotation offset - and fails most of the time under round robin, which is
 * what a regression test wants.
 *
 * Suspended rather than running, so this measures placement and nothing
 * else: a thread that never wakes cannot finish early and free its core
 * underneath the assertion.
 */
/*
 * A thread that occupies a processor without doing anything on it.
 *
 * `short_thread` cannot be used for this: it returns at once, so by the
 * time the thread being measured is created its "filler" is already DEAD
 * and the core it was meant to occupy is empty again. That is what made
 * the first version of the check below fail against the very policy it
 * was written for.
 *
 * Blocked rather than spinning, deliberately. A spinning filler would
 * compete with the thread running this test and turn a question about
 * placement into a question about scheduling; a blocked one holds its slot
 * and its core assignment and costs nothing. It is woken at the end and
 * returns from here to exit.
 */
static void parks(void *arg)
{
    (void)arg;
    thread_block();
}

static bool test_placement_avoids_a_loaded_processor(void)
{
    unsigned cores = smp_online();
    unsigned loaded;

    if (cores < 2) {
        return true;              /* nothing to choose between */
    }

    /* The suite pins itself to one core; this is about the policy that runs
     * when it is not pinned, so it turns it on and puts it back. */
    thread_place_across(cores);

    /*
     * **Loading one core and requiring the next thread to go elsewhere**,
     * rather than emptying one and requiring it to go there.
     *
     * The other way round was written first and could not work: core zero
     * carries the thread running this test, and several others carry
     * whatever the machine started with, so no core is ever *empty* and
     * the assertion was against a state that does not exist.
     *
     * Piling threads onto one core is a state this test can create
     * outright, and it is the same question asked from the other side. It
     * is deterministic for least-loaded - three extra threads make that
     * core strictly the worst choice, whatever the rotation offset - and
     * for round robin it is a one-in-four coincidence per core, so
     * repeating it for every core is what makes this a regression test
     * rather than a lucky pass.
     *
     * Blocked fillers, not spinning ones: a spinner would compete with the
     * thread running this and turn a question about placement into a
     * question about scheduling.
     */
    for (loaded = 0; loaded < cores; loaded++) {
        struct thread *fill[3];
        struct thread *placed;
        unsigned i;
        unsigned landed;

        for (i = 0; i < 3; i++) {
            fill[i] = thread_create_on(loaded, "fill", parks, NULL);

            if (fill[i] == NULL) {
                thread_place_across(1);
                return false;
            }
        }

        /* Let each reach `thread_block`: a filler still on its way to its
         * core is not yet occupying it. */
        for (i = 0; i < 64; i++) {
            thread_yield();
        }

        placed = thread_create_suspended("placed", short_thread, NULL);

        if (placed == NULL) {
            thread_place_across(1);
            return false;
        }

        landed = placed->sched.cpu;

        /*
         * Everything woken before the verdict is returned. A blocked thread
         * left behind holds a pool slot for the rest of the run, and the
         * `as:` checks would notice later and blame something else.
         */
        thread_wake(placed);

        for (i = 0; i < 3; i++) {
            thread_wake(fill[i]);
        }

        for (i = 0; i < 64; i++) {
            thread_yield();
        }

        if (landed == loaded) {
            thread_place_across(1);
            return false;
        }
    }

    thread_place_across(1);

    return true;
}

static bool test_preemption_does_not_lose_the_preempted_thread(void)
{
    /*
     * A preempted thread has its trapframe on its own exception stack and
     * resumes through the vector's epilogue rather than through
     * context_switch's caller. If either stack were wrong, it would come
     * back with a corrupted frame rather than not at all, so this checks
     * that both threads keep counting.
     */
    uint64_t deadline;
    unsigned long mine = 0;
    unsigned long theirs;
    unsigned i;

    spinner_stop = false;
    spinner_laps = 0;

    if (thread_create("spinner2", spinner, NULL) == NULL) {
        return false;
    }

    deadline = cntpct() + cntfrq() / 2;
    while (cntpct() < deadline) {
        mine++;
    }

    theirs = spinner_laps;
    spinner_stop = true;

    deadline = cntpct() + cntfrq() / 2;
    while (cntpct() < deadline) {
        /* nothing */
    }

    for (i = 0; i < 8; i++) {
        thread_yield();
    }

    /* Both made progress, which means the CPU went back and forth rather
     * than one thread being abandoned mid-switch. */
    return mine > 0 && theirs > 0;
}

/*
 * M3: synchronous IPC.
 */
static volatile cap_t server_cap;
static volatile int   server_result;
static volatile bool  server_done;

static void echo_server(void *arg)
{
    /*
     * The shape every Kosmos server will have: receive, act, reply, repeat.
     * This one answers with the request's words doubled, so a reply that
     * arrives from the wrong place is visible rather than plausible.
     */
    unsigned rounds = (unsigned)(uintptr_t)arg;
    unsigned i;

    for (i = 0; i < rounds; i++) {
        struct message msg;
        struct message reply;
        struct thread *sender;
        uint32_t w;

        server_result = ipc_receive(server_cap, &msg, &sender, false, 0);
        if (server_result != IPC_OK) {
            break;
        }

        /* Doubles every byte, which is nonsense as a payload and exactly
         * right as a test: the kernel has no opinion about these bytes, so
         * whatever comes back proves it moved them unchanged. */
        reply.tag = msg.tag + 1;
        reply.length = msg.length;
        for (w = 0; w < msg.length; w++) {
            reply.data[w] = (uint8_t)(msg.data[w] * 2);
        }

        server_result = ipc_reply(sender, &reply);
        if (server_result != IPC_OK) {
            break;
        }
    }

    server_done = true;
}

/*
 * Priority inheritance: a server runs at the band of whoever is waiting on
 * it, and gives it back when it answers.
 *
 * This is what makes it possible to have a server that is *both* the input
 * path and the print path - which the console is - without either starving
 * the machine or answering keystrokes slowly. Promoting it outright was
 * tried and starved everything it serves; inheriting means the question
 * "how important is this server" gets the only honest answer, which is that
 * it depends entirely on who is asking.
 */
static volatile unsigned inherit_seen_inside;
static volatile unsigned inherit_seen_after;
static cap_t inherit_cap;

static void inherit_server(void *arg)
{
    struct message msg, reply = { 0 };
    struct thread *sender;

    (void)arg;

    if (ipc_receive(inherit_cap, &msg, &sender, false, 0) != IPC_OK) {
        thread_exit();
    }

    /* Serving a caller from the input band, so this must read as the input
     * band and not as the LOW this thread was created with. */
    inherit_seen_inside = thread_effective_priority(thread_current());

    ipc_reply(sender, &reply);

    /* And handed straight back. */
    inherit_seen_after = thread_effective_priority(thread_current());

    thread_exit();
}

static bool test_a_server_inherits_its_callers_priority(void)
{
    struct message msg = { 0 };
    struct message reply = { 0 };
    struct thread *server;
    cap_t client_cap;
    unsigned i;

    inherit_seen_inside = 0;
    inherit_seen_after  = 99;

    client_cap = ipc_endpoint_create();
    if (client_cap < 0) {
        return false;
    }

    server = thread_create_suspended("inherit", inherit_server, NULL);
    if (server == NULL) {
        return false;
    }

    inherit_cap = ipc_cap_grant(server, client_cap);
    if (inherit_cap < 0) {
        return false;
    }

    /* Deliberately the lowest band that still runs, so a server that did
     * not inherit would be visibly below its caller. */
    thread_set_priority(server, SCHED_PRIO_LOW);
    thread_wake(server);

    /* And the caller is the most urgent thing on the machine. */
    thread_set_priority(thread_current(), SCHED_PRIO_INPUT);

    msg.tag = 1;
    msg.length = 0;

    if (ipc_call(client_cap, &msg, &reply) != IPC_OK) {
        thread_set_priority(thread_current(), SCHED_PRIO_NORMAL);
        return false;
    }

    thread_set_priority(thread_current(), SCHED_PRIO_NORMAL);

    for (i = 0; i < 100000u && inherit_seen_after == 99; i++) {
        thread_yield();
    }

    return inherit_seen_inside == SCHED_PRIO_INPUT
        && inherit_seen_after  == SCHED_PRIO_LOW;
}

static bool test_ipc_call_and_reply(void)
{
    struct message msg = { 0 };
    struct message reply = { 0 };
    cap_t client_cap;
    struct thread *server;
    unsigned i;

    server_done = false;
    server_result = IPC_OK;

    client_cap = ipc_endpoint_create();
    if (client_cap < 0) {
        return false;
    }

    /* Suspended, because it reads server_cap and a thread is runnable the
     * instant it exists. Started first, it reads whatever that global last
     * held, fails its receive and exits, and the caller below then blocks
     * against a server that is already gone. */
    server = thread_create_suspended("echo", echo_server, (void *)(uintptr_t)1);
    if (server == NULL) {
        return false;
    }

    /* The server gets its own index for the same endpoint. The two are
     * unrelated on purpose: an index means nothing outside its own table. */
    server_cap = ipc_cap_grant(server, client_cap);
    if (server_cap < 0) {
        return false;
    }

    thread_wake(server);

    msg.tag = 7;
    msg.length = 16;
    for (i = 0; i < 16; i++) {
        msg.data[i] = (uint8_t)(i + 1);
    }

    if (ipc_call(client_cap, &msg, &reply) != IPC_OK) {
        return false;
    }

    if (reply.tag != 8) {
        return false;
    }

    if (reply.length != 16) {
        return false;
    }

    for (i = 0; i < 16; i++) {
        if (reply.data[i] != (uint8_t)((i + 1) * 2)) {
            return false;
        }
    }

    for (i = 0; i < 8 && !server_done; i++) {
        thread_yield();
    }

    (void)ipc_endpoint_destroy(client_cap);
    return server_done && server_result == IPC_OK;
}

static bool test_ipc_works_in_both_arrival_orders(void)
{
    /*
     * Rendezvous has two paths and only one of them gets exercised by
     * accident. Sender first means the message waits on the endpoint;
     * receiver first means it is handed straight over. Both have to work,
     * and the second is the one the fast path depends on.
     *
     * The first call in this test arrives before the server has ever run, so
     * it takes the sender-first path. Yielding first lets the server reach
     * its receive, so the second call takes the receiver-first path.
     */
    struct message msg = { 0 };
    struct message reply = { 0 };
    cap_t cap;
    struct thread *server;
    unsigned i;
    bool ok;

    server_done = false;
    cap = ipc_endpoint_create();
    if (cap < 0) {
        return false;
    }

    server = thread_create_suspended("echo2", echo_server,
                                     (void *)(uintptr_t)2);
    if (server == NULL) {
        return false;
    }
    server_cap = ipc_cap_grant(server, cap);
    thread_wake(server);

    /* Sender first: the server has not run at all yet. */
    msg.tag = 1;
    msg.length = 1;
    msg.data[0] = 10;
    ok = (ipc_call(cap, &msg, &reply) == IPC_OK) && reply.data[0] == 20;

    /* Receiver first: give the server a chance to be waiting. */
    for (i = 0; i < 4; i++) {
        thread_yield();
    }

    msg.tag = 2;
    msg.length = 1;
    msg.data[0] = 30;
    ok = ok && (ipc_call(cap, &msg, &reply) == IPC_OK) && reply.data[0] == 60;

    for (i = 0; i < 8 && !server_done; i++) {
        thread_yield();
    }

    (void)ipc_endpoint_destroy(cap);
    return ok;
}

static volatile int blocked_result;
static volatile bool blocked_returned;

static void blocked_caller(void *arg)
{
    struct message msg = { 0 };
    struct message reply = { 0 };
    cap_t cap = (cap_t)(intptr_t)arg;

    msg.tag = 1;
    blocked_result = ipc_call(cap, &msg, &reply);
    blocked_returned = true;
}

static bool test_destroying_an_endpoint_wakes_the_blocked(void)
{
    /*
     * The trap roadmap.md names for this milestone, and the one that decides
     * whether a server can ever be restarted.
     *
     * A thread blocked in a call on an endpoint that is then destroyed has
     * to come back with an error. If it does not, it waits forever for a
     * server that no longer exists, and every recovery story in design.md
     * §10 rests on this working.
     */
    cap_t cap;
    struct thread *caller;
    cap_t caller_cap;
    unsigned i;

    blocked_returned = false;
    blocked_result = IPC_OK;

    cap = ipc_endpoint_create();
    if (cap < 0) {
        return false;
    }

    caller = thread_create_suspended("caller", blocked_caller, NULL);
    if (caller == NULL) {
        return false;
    }

    caller_cap = ipc_cap_grant(caller, cap);
    if (caller_cap < 0) {
        return false;
    }

    /* The thread was created before its capability index was known, so hand
     * it over through the argument slot the entry function reads. Safe
     * because it is suspended: started first, it would have read the old
     * value before this line ran. */
    test_context_set_arg(&caller->ctx,
                         (uint64_t)(uintptr_t)(intptr_t)caller_cap);
    thread_wake(caller);

    /* Let it block. Nobody will ever receive. */
    for (i = 0; i < 4; i++) {
        thread_yield();
    }

    if (blocked_returned) {
        return false;   /* it did not block at all */
    }

    if (ipc_endpoint_destroy(cap) != IPC_OK) {
        return false;
    }

    for (i = 0; i < 8 && !blocked_returned; i++) {
        thread_yield();
    }

    return blocked_returned && blocked_result == IPC_ERR_GONE;
}

static bool test_a_stale_capability_fails(void)
{
    /*
     * Endpoint slots are reused. Without a generation number, a capability
     * that outlived its endpoint would quietly start naming whatever was
     * created next in the same slot, which is a thread reaching something it
     * was never handed. That is the exact property capabilities exist to
     * prevent, so it is worth an explicit test rather than trust.
     */
    struct message msg = { 0 };
    struct message reply = { 0 };
    cap_t first;
    cap_t second;

    first = ipc_endpoint_create();
    if (first < 0) {
        return false;
    }

    if (ipc_endpoint_destroy(first) != IPC_OK) {
        return false;
    }

    /* Very likely the same slot, since it was just freed. */
    second = ipc_endpoint_create();
    if (second < 0) {
        return false;
    }

    /*
     * Plant a stale capability in a slot of its own rather than over the top
     * of the live one, so the endpoint created above stays reachable and can
     * be destroyed afterwards. Overwriting the live slot leaves an endpoint
     * that nothing can name, which is a leak in the test rather than a
     * finding about the kernel.
     */
    {
        struct thread *self = thread_current();
        cap_t spare = -1;
        bool stale_rejected;
        cap_t i;

        for (i = 0; i < CAPS_PER_THREAD; i++) {
            if (self->caps[i].endpoint == NULL) {
                spare = i;
                break;
            }
        }

        if (spare < 0) {
            return false;
        }

        /* The same endpoint, named with the generation it had before the
         * destroy. That is precisely what a capability held across a
         * server restart looks like. */
        self->caps[spare].endpoint = self->caps[second].endpoint;
        self->caps[spare].generation = self->caps[second].generation - 1;

        stale_rejected = ipc_call(spare, &msg, &reply) == IPC_ERR_BAD_CAP;

        self->caps[spare].endpoint = NULL;

        return stale_rejected && ipc_endpoint_destroy(second) == IPC_OK;
    }
}

static bool test_a_capability_index_out_of_range_fails(void)
{
    struct message msg = { 0 };
    struct message reply = { 0 };

    return ipc_call(-1, &msg, &reply) == IPC_ERR_BAD_CAP
        && ipc_call(CAPS_PER_THREAD, &msg, &reply) == IPC_ERR_BAD_CAP
        && ipc_call(0, &msg, &reply) == IPC_ERR_BAD_CAP;   /* nothing installed */
}

/*
 * A non-blocking receive on an empty endpoint.
 *
 * The whole point of the flag is that this call *returns*, so the test is
 * that reaching the line after it happens at all. There is no way to assert
 * "did not block" directly: ignoring the flag parks this thread on an
 * endpoint nobody will ever send to, and what comes out is a panic from the
 * scheduler with nothing left to run - not a failed assertion.
 *
 * Worth knowing when this one breaks: a panic here, with the test count
 * stopping around this number, is this test.
 */
static bool test_a_nonblocking_receive_returns_empty(void)
{
    cap_t cap = ipc_endpoint_create();
    struct message msg;
    struct thread *sender = NULL;
    int status;

    if (cap < 0) {
        return false;
    }

    status = ipc_receive(cap, &msg, &sender, true, 0);
    ipc_endpoint_destroy(cap);

    return status == IPC_NO_MESSAGE;
}

/*
 * A receive with a deadline comes back when the deadline passes.
 *
 * Nobody sends anything to this endpoint, so the only way out is the timer -
 * which is the whole point of the argument. Before it existed, a server that
 * wanted to be somewhere else in a millisecond had to spin or sleep, and
 * sleeping made it deaf: the audio server answered its clients a whole tick
 * late, so a program handing it twelve periods a pass spent 45 ms on twelve
 * round trips and played its music at two thirds speed.
 *
 * **In a thread of its own**, because a blocking receive on the only
 * runnable thread is `thread_block: every thread is blocked` - which is what
 * the first version of this test produced, and is the same reason every
 * other blocking test here creates one.
 */
static cap_t          deadline_cap;
static volatile int   deadline_status;
static volatile unsigned long deadline_waited;

static void deadline_thread(void *arg)
{
    struct message msg;
    struct thread *sender = NULL;
    unsigned long began = hal_ticks();

    (void)arg;

    int status = ipc_receive(deadline_cap, &msg, &sender, false,
                             (unsigned long)3);

    /*
     * The measurement first and the flag last, because the flag is what the
     * waiting thread spins on. Written the other way round - which is how
     * this was first - the test could read `deadline_waited` before this
     * thread had written it, and passed or failed depending on what the
     * previous test happened to leave in it. It passed when a test that did
     * more yielding ran alongside, which is exactly how a race advertises
     * itself.
     */
    deadline_waited = hal_ticks() - began;
    deadline_status = status;
}

static bool test_a_receive_with_a_deadline_gives_up(void)
{
    cap_t cap = ipc_endpoint_create();
    struct thread *t;
    unsigned long limit;
    unsigned i;

    if (cap < 0) {
        return false;
    }

    deadline_status = 12345;            /* neither an error nor a success */
    deadline_waited = 0;

    t = thread_create("deadline", deadline_thread, NULL);

    if (t == NULL) {
        ipc_endpoint_destroy(cap);
        return false;
    }

    /*
     * The helper needs its *own* index for this endpoint.
     *
     * Capabilities are per-thread - `resolve` reads `t->caps[index]` - so
     * handing a created thread the creator's number gets `IPC_ERR_BAD_CAP`
     * and a test that fails for a reason that has nothing to do with what
     * it is testing. Which is how the first version of this failed.
     *
     * Granted after `thread_create` and before the first `thread_yield`,
     * because a created thread does not run until something yields to it.
     */
    deadline_cap = ipc_cap_grant(t, cap);

    if (deadline_cap < 0) {
        ipc_endpoint_destroy(cap);
        return false;
    }

    /*
     * Waited out in *ticks*, not in iterations.
     *
     * This counted to a hundred thousand yields, which sounds generous and
     * is not a duration: a yield with nothing else runnable is quick, so the
     * count could run out before the three ticks being waited for had passed
     * - and the test failed for having been in a hurry. It passed most runs
     * and failed some, which is the worst way for a test to be wrong.
     */
    limit = hal_ticks() + 100;          /* 400 ms, far past three ticks */

    while (deadline_status == 12345 && hal_ticks() < limit) {
        thread_yield();
    }

    /* And let it run off the end of its function before its endpoint goes. */
    for (i = 0; i < 8; i++) {
        thread_yield();
    }

    ipc_endpoint_destroy(cap);

    if (deadline_status != IPC_NO_MESSAGE) {
        return false;
    }

    /* It waited, and it did not wait for ever. Two ticks is the floor
     * because the first may land almost immediately. */
    return deadline_waited >= 2 && deadline_waited < 200;
}

static bool test_endpoints_are_reclaimed(void)
{
    unsigned before = ipc_endpoints_in_use();
    cap_t cap = ipc_endpoint_create();

    if (cap < 0 || ipc_endpoints_in_use() != before + 1) {
        return false;
    }

    if (ipc_endpoint_destroy(cap) != IPC_OK) {
        return false;
    }

    return ipc_endpoints_in_use() == before;
}

/*
 * M3: the exception stack.
 */
extern char __exception_stack_bottom[], __exception_stack_top[];
extern char __stack_bottom[], __stack_top[];

#if defined(__aarch64__)
/*
 * Which stack the kernel itself runs on, and AArch64's alone.
 *
 * x86 has no stack selector to read. The equivalent question there is
 * whether the TSS names a separate stack for an entry from ring 3, which
 * `gdt.c` answers and which the test below - about the handler's stack -
 * checks the *effect* of on both boards.
 */
static bool test_kernel_runs_on_sp_el0(void)
{
    /* SPSel bit 0: 0 means SP_EL0 is the current stack pointer. The whole
     * separation rests on this being 0 in ordinary code. */
    uint64_t spsel;
    __asm__ volatile("mrs %0, spsel" : "=r"(spsel));
    return (spsel & 1) == 0;
}
#endif /* __aarch64__ */

/*
 * The two about a *separate* exception stack, and they run on both boards
 * now - which they did not, and the reason is worth keeping.
 *
 * AArch64 gets this from the architecture: the kernel runs on SP_EL0 on
 * purpose, so every exception switches to SP_EL1 and the handler stands on
 * a stack that is not the one that faulted. **x86 has to be told.** A fault
 * from ring 0 stays on the faulting stack unless the gate names an entry in
 * the interrupt stack table, and every gate named none - so a kernel stack
 * overflow pushed the fault frame into the guard page, faulted again
 * delivering that, and triple-faulted the machine. It reset with nothing
 * printed, which is the exact failure the exception dump exists to
 * eliminate. `trap.c` now puts #PF and #DF on `tss.ist[0]`.
 *
 * **The fault below is a store and not an undefined instruction**, and that
 * is the whole of what x86 changed here. An IST entry belongs to a
 * *vector*, and only the two that need it have one - an undefined
 * instruction is a fault by code that still has a working stack and is
 * better handled on it. So a test asking "does the handler have its own
 * stack" has to fault the way a stack overflow faults, which is a memory
 * access. On AArch64 it makes no difference; every exception lands on
 * SP_EL1 whatever caused it.
 */
static bool test_handler_runs_on_the_exception_stack(void)
{
    /*
     * Direct evidence that the two stacks are actually separate, rather than
     * the code merely looking as though it set them up that way.
     */
    struct fault_info f;

    FAULT_EXPECT({ store_to(0, 1); });

    if (!fault_expect_end(&f)) {
        return false;
    }

    return f.handler_sp > (uint64_t)(uintptr_t)__exception_stack_bottom
        && f.handler_sp <= (uint64_t)(uintptr_t)__exception_stack_top;
}

static volatile int recursion_guard;

/*
 * GCC is right that this never returns, and that is the point: it is
 * terminated by a hardware fault, which no amount of static analysis can
 * see. The warning is correct and the code is deliberate, so it is silenced
 * here and nowhere else.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"

static uint64_t burn_stack(uint64_t depth)
{
    /*
     * Recurses until the stack runs into its guard page. The volatile
     * counter and the returned sum are there so the compiler cannot turn
     * this into a loop or prove anything about where it ends: it has to
     * actually build a frame each time.
     */
    volatile uint64_t pad[16];
    uint64_t i;
    uint64_t sum = depth;

    recursion_guard++;

    for (i = 0; i < 16; i++) {
        pad[i] = depth + i;
    }

    sum += burn_stack(depth + 1);

    for (i = 0; i < 16; i++) {
        sum += pad[i];
    }

    return sum;
}

#pragma GCC diagnostic pop

static bool test_a_stack_overflow_is_survivable(void)
{
    /*
     * The reason the exception stack exists, tested end to end.
     *
     * Before this, an overflow was a double fault: the handler built its
     * 288-byte frame on the stack that had just run out, faulted again inside
     * the vector, and the kernel hung with nothing printed. Now the fault
     * lands on a different stack, is recognised, and unwinds back here.
     *
     * Stepping ELR would be no use: resuming the faulting instruction just
     * recurses into the guard page again. This is what fault_expect_unwind is
     * for, and it is the case testing.md §18.2 had in mind.
     */
    jmp_buf recover;
    volatile bool unwound = false;
    volatile uint64_t sp_after;

    recursion_guard = 0;

    if (setjmp(recover) == 0) {
        fault_expect_unwind(recover);
        (void)burn_stack(0);
        /* Unreachable: the recursion cannot return. */
    } else {
        unwound = true;
    }

    if (!fault_expect_end(NULL)) {
        return false;
    }

    /* And the stack pointer is back where setjmp saw it, not thousands of
     * frames down where the fault happened. */
    sp_after = test_stack_pointer();

    /*
     * The depth bound is deliberately loose. How many frames fit in 16 KB
     * depends on what the compiler decided this function's frame should be,
     * and that is not the property under test: what matters is that it
     * really recursed and consumed a stack rather than faulting immediately.
     * It happens to reach exactly 100, which is about 163 bytes a frame.
     */
    return unwound
        && recursion_guard > 10
        && sp_after > (uint64_t)(uintptr_t)__stack_bottom
        && sp_after <= (uint64_t)(uintptr_t)__stack_top;
}

/*
 * M1: the physical page allocator.
 *
 * The panic paths (a misaligned free, an out-of-range free, a double free)
 * are deliberately untested. They halt the kernel by design, which is the
 * right behaviour for a programmer error and the wrong thing to assert on
 * from inside the kernel being tested. Turning them into return codes just
 * to make them testable would invite callers to ignore them.
 */
static bool test_pmm_alloc_is_page_aligned_and_in_ram(void)
{
    void *p = pmm_alloc_page();
    bool ok;

    if (p == NULL) {
        return false;
    }

    ok = ((uintptr_t)p & PAGE_MASK) == 0;
    pmm_free_page(p);
    return ok;
}

static bool test_pmm_two_allocations_differ(void)
{
    void *a = pmm_alloc_page();
    void *b = pmm_alloc_page();
    bool ok = a != NULL && b != NULL && a != b;

    if (a != NULL) { pmm_free_page(a); }
    if (b != NULL) { pmm_free_page(b); }
    return ok;
}

static bool test_pmm_count_tracks_alloc_and_free(void)
{
    size_t before = pmm_free_pages();
    void *p = pmm_alloc_page();

    if (p == NULL || pmm_free_pages() != before - 1) {
        return false;
    }

    pmm_free_page(p);
    return pmm_free_pages() == before;
}

static bool test_pmm_page_is_writable(void)
{
    volatile uint64_t *p = pmm_alloc_page();
    bool ok;

    if (p == NULL) {
        return false;
    }

    /* Both ends, so a page handed out with the wrong size shows up here
     * rather than as corruption of whatever lives next to it. */
    p[0] = 0x0123456789abcdefULL;
    p[PAGE_SIZE / sizeof(uint64_t) - 1] = 0xfedcba9876543210ULL;

    ok = p[0] == 0x0123456789abcdefULL
      && p[PAGE_SIZE / sizeof(uint64_t) - 1] == 0xfedcba9876543210ULL;

    pmm_free_page((void *)p);
    return ok;
}

static bool test_pmm_kernel_pages_are_not_free(void)
{
    /* The image and the bitmap are inside RAM but must never be handed out.
     * If they were, the count would equal the total. */
    return pmm_free_pages() < pmm_total_pages();
}

static bool test_pmm_exhaustion_returns_null(void)
{
    /*
     * Allocates every remaining page, checks the count reaches zero and the
     * next call returns NULL, then gives them all back.
     *
     * The pages are threaded into a list through their own first eight
     * bytes, which is the only way to remember 130,000 addresses without
     * somewhere to put them. It also proves every page handed out is
     * genuinely writable.
     */
    size_t before = pmm_free_pages();
    size_t taken = 0;
    void *head = NULL;
    void *p;
    bool ok;

    while ((p = pmm_alloc_page()) != NULL) {
        *(void **)p = head;
        head = p;
        taken++;
    }

    ok = (taken == before)
      && (pmm_free_pages() == 0)
      && (pmm_alloc_page() == NULL);

    while (head != NULL) {
        void *next = *(void **)head;
        pmm_free_page(head);
        head = next;
    }

    return ok && pmm_free_pages() == before;
}

/*
 * M2: the heap and snprintf.
 */
static bool test_heap_alloc_is_aligned_and_usable(void)
{
    /* 16 bytes, because a double and a long double both want it and Lua puts
     * both inside its values. An 8-aligned heap works until something takes
     * a 16-byte load across the boundary. */
    void *a = malloc(1);
    void *b = malloc(17);
    void *c = malloc(4096);
    bool ok = a != NULL && b != NULL && c != NULL
           && ((uintptr_t)a % 16) == 0
           && ((uintptr_t)b % 16) == 0
           && ((uintptr_t)c % 16) == 0;

    free(a); free(b); free(c);
    return ok;
}

static bool test_heap_coalesces_in_both_directions(void)
{
    /*
     * The property the whole allocator rests on. Three neighbours freed
     * middle-last must end as one block, or the heap fills with holes that
     * are physically adjacent and individually too small, and Lua's GC can
     * never get memory back.
     */
    size_t before = heap_used();
    void *a = malloc(2048);
    void *b = malloc(2048);
    void *c = malloc(2048);
    void *big;
    bool ok;

    if (a == NULL || b == NULL || c == NULL) {
        return false;
    }

    free(a);
    free(c);
    free(b);   /* last, so it has to merge with a free block on each side */

    big = malloc(6000);
    ok = big != NULL;
    free(big);

    return ok && heap_used() == before;
}

static bool test_heap_realloc_preserves_contents(void)
{
    unsigned char *p = malloc(64);
    unsigned char *q;
    bool ok = true;
    int i;

    if (p == NULL) {
        return false;
    }

    for (i = 0; i < 64; i++) {
        p[i] = (unsigned char)(i * 7);
    }

    q = realloc(p, 4096);
    if (q == NULL) {
        free(p);
        return false;
    }

    for (i = 0; i < 64; i++) {
        if (q[i] != (unsigned char)(i * 7)) {
            ok = false;
        }
    }

    free(q);
    return ok;
}

static bool test_heap_exhaustion_returns_null(void)
{
    /* Bigger than the whole heap. Returning NULL rather than a pointer into
     * nothing is what lets Lua's allocator report an out-of-memory error
     * instead of corrupting the heap. */
    void *p = malloc(heap_size() + 1);

    if (p != NULL) {
        free(p);
        return false;
    }

    return true;
}

static bool str_is(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

static bool test_snprintf_integers_and_strings(void)
{
    char b[64];

    snprintf(b, sizeof(b), "%d", -42);            if (!str_is(b, "-42"))      return false;
    snprintf(b, sizeof(b), "%5d|", 42);           if (!str_is(b, "   42|"))   return false;
    snprintf(b, sizeof(b), "%-5d|", 42);          if (!str_is(b, "42   |"))   return false;
    snprintf(b, sizeof(b), "%05d", 42);           if (!str_is(b, "00042"))    return false;
    snprintf(b, sizeof(b), "%x", 0xdeadbeefu);    if (!str_is(b, "deadbeef")) return false;
    snprintf(b, sizeof(b), "%s!", "hi");          if (!str_is(b, "hi!"))      return false;
    snprintf(b, sizeof(b), "%.2s", "hello");      if (!str_is(b, "he"))       return false;
    snprintf(b, sizeof(b), "%lld", 9223372036854775807LL);
    if (!str_is(b, "9223372036854775807")) return false;

    /* The one that overflows if the magnitude is taken by plain negation. */
    snprintf(b, sizeof(b), "%lld", (long long)-9223372036854775807LL - 1);
    return str_is(b, "-9223372036854775808");
}

static bool test_snprintf_truncates_and_reports_the_full_length(void)
{
    /* Returning what it would have needed is how a caller sizes a buffer,
     * and writing the terminator inside the cap is what keeps the result a
     * string. */
    char b[5];
    int n = snprintf(b, sizeof(b), "%s", "abcdefgh");

    return n == 8 && str_is(b, "abcd");
}

/*
 * A float in a column.
 *
 * `format_double` applied no width at all, which nothing failed over: every
 * table of numbers any program printed simply ran together, and that reads
 * as somebody's formatting choice rather than as a missing feature. It was
 * found by looking at a benchmark's output and wondering why the columns
 * were not columns.
 */
static bool test_snprintf_float_width(void)
{
    char b[64];

    snprintf(b, sizeof(b), "%8.2f|",  1.5);    if (!str_is(b, "    1.50|")) return false;
    snprintf(b, sizeof(b), "%-8.2f|", 1.5);    if (!str_is(b, "1.50    |")) return false;
    snprintf(b, sizeof(b), "%8.0f|",  42.0);   if (!str_is(b, "      42|")) return false;
    snprintf(b, sizeof(b), "%-8.0f|", 42.0);   if (!str_is(b, "42      |")) return false;

    /* Narrower than the number: the width is a minimum, never a limit. */
    snprintf(b, sizeof(b), "%2.2f|",  1234.5); if (!str_is(b, "1234.50|")) return false;

    /* Zero padding goes after the sign. */
    snprintf(b, sizeof(b), "%08.2f|", -3.5);   if (!str_is(b, "-0003.50|")) return false;
    snprintf(b, sizeof(b), "%08.2f|",  3.5);   if (!str_is(b, "00003.50|")) return false;

    /* And %g, which is the one Lua reaches for. */
    snprintf(b, sizeof(b), "%10.14g|", 1.5);   if (!str_is(b, "       1.5|")) return false;

    return true;
}

static bool test_snprintf_floats(void)
{
    char b[64];

    /* Lua's default number format is %.14g, so that is the one that matters. */
    snprintf(b, sizeof(b), "%.14g", 1.5);      if (!str_is(b, "1.5"))    return false;
    snprintf(b, sizeof(b), "%.14g", 100.0);    if (!str_is(b, "100"))    return false;
    snprintf(b, sizeof(b), "%.14g", 0.5);      if (!str_is(b, "0.5"))    return false;
    snprintf(b, sizeof(b), "%.14g", -0.25);    if (!str_is(b, "-0.25"))  return false;
    snprintf(b, sizeof(b), "%.14g", 0.0);      if (!str_is(b, "0"))      return false;
    snprintf(b, sizeof(b), "%.3f",  1.5);      if (!str_is(b, "1.500"))  return false;
    snprintf(b, sizeof(b), "%.14g", 1e20);     if (!str_is(b, "1e+20"))  return false;
    snprintf(b, sizeof(b), "%.14g", 1e-7);     if (!str_is(b, "1e-07"))  return false;

    return true;
}

static bool test_our_math_matches_its_definition(void)
{
    int e = 999;
    double m;

    if (floor(2.7) != 2.0 || floor(-2.7) != -3.0) return false;
    if (ceil(2.1)  != 3.0 || ceil(-2.1)  != -2.0) return false;
    if (trunc(2.7) != 2.0 || trunc(-2.7) != -2.0) return false;
    if (fabs(-3.5) != 3.5 || fabs(3.5)   != 3.5)  return false;

    /* frexp returns a mantissa in [0.5, 1) and an exponent that rebuilds the
     * value exactly. 8.0 is 0.5 * 2^4. */
    m = frexp(8.0, &e);
    if (m != 0.5 || e != 4) return false;

    m = frexp(0.0, &e);
    if (m != 0.0 || e != 0) return false;

    if (ldexp(0.5, 4) != 8.0) return false;
    if (ldexp(1.0, 0) != 1.0) return false;

    /*
     * The vendored half is *not* checked here any more, and moving it was
     * overdue rather than forced.
     *
     * This called `pow` and `fmod` so that a broken libm link showed up
     * here rather than inside Lua's arithmetic - which was the right place
     * when Lua ran at EL1. It has not for a long time: the comment thirty
     * lines below this one says so. The kernel has no floating point in it
     * by construction, so a `pow` it never calls was being linked into the
     * test image for the sake of one assertion about somebody else's code.
     *
     * `user/tests/luatest.lua`'s R_MATH role checks it now, through Lua,
     * at the privilege level Lua runs at, against musl rather than newlib.
     */
    return true;
}

/*
 * M2 and M3, from where Lua actually runs.
 *
 * These were an embedded `lua_State` at EL1 and a `lua_says_true` helper
 * that ran a chunk inside the kernel. The kernel has no Lua in it any more,
 * so each one is a process now: `user/tests/luatest.lua` holds the
 * assertions, one role per test, and what is left here is the driver that
 * starts a role and reads its exit code.
 *
 * What is lost is a little directness - a failure says which role failed and
 * prints the message, rather than being a C expression the debugger can stop
 * on. What is gained is that the thing being tested is the thing that ships:
 * the same interpreter, the same libc, the same page tables, reached through
 * the same syscalls as the shell.
 */

/* Has to match LUATEST_BASE in user/init/main.c. A boot word at or above it
 * selects the test chunk instead of init. */
#define LUATEST_BASE    1000UL

/*
 * How long a role gets before it is called hung.
 *
 * Generous, because the heaviest of them allocates four thousand tables and
 * collects them. It bounds a hang into a failed test rather than into the
 * host runner's timeout, which is the difference between "this test broke"
 * and "something broke".
 */
#define LUATEST_SLICES  400000u

static bool luatest_role(unsigned long role)
{
    extern const unsigned char init_image[];
    extern const unsigned long init_image_len;
    unsigned before = process_count();
    struct process *p;
    unsigned i;
    int code;

    p = process_create("t-lua", init_image, (size_t)init_image_len,
                       LUATEST_BASE + role);

    if (p == NULL) {
        return false;
    }

    /*
     * Given the console so that a failure says what it was. Nothing is
     * printed on the way through; `user/init/main.c` prints only the error
     * that unwound out of the chunk, and that line is not TAP, so the runner
     * ignores it and a human reading the output gets the reason.
     *
     * Children spawned by the role are not given it, deliberately: a test
     * that needs a server to print is a test that has stopped being about
     * the server.
     */
    process_grant_console(p);
    process_start(p);

    for (i = 0; i < LUATEST_SLICES && !p->exited; i++) {
        thread_yield();
    }

    if (!p->exited) {
        return false;
    }

    code = p->exit_code;
    process_reap(p);

    /*
     * And that it left nothing behind. A role that spawns servers waits for
     * every one of them, so the pool has to be back where it started; a
     * server still sitting in a receive would show up here rather than as
     * the next test failing to find a slot.
     */
    return code == 0 && process_count() == before;
}

static bool test_lua_arithmetic(void)          { return luatest_role(0); }
static bool test_lua_floats(void)              { return luatest_role(1); }
static bool test_lua_strings_and_tables(void)  { return luatest_role(2); }
static bool test_lua_closures(void)            { return luatest_role(3); }
static bool test_lua_coroutines(void)          { return luatest_role(4); }
static bool test_lua_errors_are_caught(void)   { return luatest_role(5); }
static bool test_lua_errors_nest(void)         { return luatest_role(6); }
static bool test_lua_math_library(void)        { return luatest_role(7); }
static bool test_lua_gc_reclaims(void)         { return luatest_role(8); }
static bool test_lua_dangerous_libs_absent(void) { return luatest_role(9); }
static bool test_lua_refuses_bytecode(void)    { return luatest_role(10); }

static bool test_a_process_answers_over_ipc(void)      { return luatest_role(11); }
static bool test_a_lua_table_survives_ipc(void)        { return luatest_role(13); }
static bool test_the_serialiser_refuses_what_cannot_cross(void)
                                                       { return luatest_role(15); }
static bool test_a_capability_can_be_passed_in_a_message(void)
                                                       { return luatest_role(16); }
static bool test_a_capability_that_is_not_held_cannot_be_sent(void)
                                                       { return luatest_role(20); }
static bool test_lua_ipc_errors_are_reported(void)     { return luatest_role(22); }
static bool test_gfx_surfaces(void)                    { return luatest_role(24); }
static bool test_gfx_alpha_is_exact(void)              { return luatest_role(25); }
static bool test_a_process_without_the_screen(void)    { return luatest_role(26); }
static bool test_gfx_text(void)                        { return luatest_role(27); }

/*
 * Shared memory: two processes on one set of pages, and the pages back
 * afterwards.
 *
 * The page count is the whole point of doing this here rather than inside
 * the role. A region's pages are freed when the last capability to it is
 * dropped, and the last capability is dropped when the last of the two
 * processes is reaped - so nothing running inside either of them can see
 * the number that matters. From out here both are gone.
 *
 * What it catches, in the form it caught it: shared regions used to be
 * mapped in the window `SYS_MAP` hands out, whose pages a process frees on
 * the way out because it allocated them. Two processes mapping one region
 * freed its pages twice. The panic was the lucky outcome - the pages could
 * as easily have been handed to somebody else while the other process was
 * still drawing into them.
 *
 * An equality, not a bound. "No worse than before" would pass while a
 * region leaked its pages every time a window opened, which is the failure
 * that has no symptom until the machine runs out.
 */
/*
 * The block device.
 *
 * Everything about virtio-blk in this kernel was written against the
 * specification from knowledge rather than from a copy of `virtio_blk.h`,
 * which is not on the machine it was written on. That makes these tests
 * load-bearing in a way most are not: they are not confirming a design, they
 * are establishing that the request header's field offsets, the descriptor
 * chain's direction flags and the unit the sector is counted in are all
 * right.
 *
 * A write followed by a read is what does it. Every one of those details
 * lies on the path between the two, and no combination of wrong ones
 * produces the bytes that went in.
 *
 */
static uint8_t blk_out[HAL_BLK_SECTOR];
static uint8_t blk_in[HAL_BLK_SECTOR];

static bool test_block_device_is_present(void)
{
    struct blkdev dev;

    if (!hal_blk_init(&dev)) {
        return false;
    }

    /* The runner attaches a 1 MB disk. Not an exact figure here, because
     * the test should not fail when that changes - only that the capacity
     * is a real number and the sector size is what virtio counts in. */
    return dev.sectors > 0 && dev.sector_size == HAL_BLK_SECTOR;
}

static bool test_block_write_then_read(void)
{
    unsigned i;

    /* A pattern where every byte depends on its position, so a read that
     * lands on the wrong sector, or is short, or is offset by a few bytes,
     * fails rather than matching by luck. A constant fill would pass all
     * three of those. */
    for (i = 0; i < HAL_BLK_SECTOR; i++) {
        blk_out[i] = (uint8_t)(i * 7 + 13);
        blk_in[i]  = 0;
    }

    if (!hal_blk_write(1, blk_out, HAL_BLK_SECTOR)) {
        return false;
    }

    if (!hal_blk_read(1, blk_in, HAL_BLK_SECTOR)) {
        return false;
    }

    for (i = 0; i < HAL_BLK_SECTOR; i++) {
        if (blk_in[i] != blk_out[i]) {
            return false;
        }
    }

    return true;
}

static bool test_block_sectors_are_distinct(void)
{
    unsigned i;

    /*
     * Sector 2 is not sector 1.
     *
     * The check that catches the sector field being ignored, or being
     * counted in the wrong unit. Without it a driver that always addresses
     * sector 0 passes the test above perfectly: it writes the pattern
     * somewhere and reads it back from the same somewhere.
     */
    for (i = 0; i < HAL_BLK_SECTOR; i++) {
        blk_out[i] = 0xa5;
    }

    if (!hal_blk_write(2, blk_out, HAL_BLK_SECTOR)) {
        return false;
    }

    if (!hal_blk_read(1, blk_in, HAL_BLK_SECTOR)) {
        return false;
    }

    /* Sector 1 still holds what the previous test put there. */
    for (i = 0; i < HAL_BLK_SECTOR; i++) {
        if (blk_in[i] != (uint8_t)(i * 7 + 13)) {
            return false;
        }
    }

    return true;
}

static bool test_block_refuses_past_the_end(void)
{
    struct blkdev dev;

    if (!hal_blk_init(&dev)) {
        return false;
    }

    /* Reading past the disk must be refused here rather than handed to the
     * device, which is entitled to do anything with it. */
    return !hal_blk_read(dev.sectors, blk_in, HAL_BLK_SECTOR)
        && !hal_blk_read(dev.sectors - 1, blk_in, HAL_BLK_SECTOR * 2)
        && !hal_blk_read(0, blk_in, HAL_BLK_SECTOR / 2);  /* not whole sectors */
}

static bool test_gfx_triangles(void)                   { return luatest_role(30); }
static bool test_g3d_orientation(void)                 { return luatest_role(31); }
static bool test_kill_is_parent_only(void)             { return luatest_role(32); }
static bool test_cap_release_frees_slots(void)         { return luatest_role(35); }
static bool test_inflate_round_trip(void)              { return luatest_role(36); }
static bool test_pdf_scanner(void)                     { return luatest_role(37); }

/*
 * A console write must carry text and nothing else. The role spawns a writer
 * holding five capabilities and asserts that four short writes arrive with
 * none of them attached; see the note beside it for what used to happen.
 */
static bool test_console_write_carries_no_capability(void) { return luatest_role(38); }

static bool test_shared_memory_is_freed_once(void)
{
    size_t before = pmm_free_pages();

    if (!luatest_role(28)) {
        return false;
    }

    return pmm_free_pages() == before;
}

/*
 * The one role the process cannot drive on its own.
 *
 * `roadmap.md` calls this M3's trap: a server blocked in receive has to be
 * woken with an error when its endpoint is destroyed, or it waits for ever
 * and can never be restarted. There is no syscall that destroys an endpoint,
 * and there should not be one yet, so the destruction happens here and what
 * is checked is that the error crosses back out to EL0 and reaches Lua.
 */
static bool test_destroying_an_endpoint_reaches_a_process(void)
{
    extern const unsigned char init_image[];
    extern const unsigned long init_image_len;
    unsigned before = process_count();
    struct process *p;
    cap_t ep;
    unsigned i;
    int code;

    ep = ipc_endpoint_create();
    if (ep < 0) {
        return false;
    }

    p = process_create("t-blocked", init_image, (size_t)init_image_len,
                       LUATEST_BASE + 23);

    if (p == NULL) {
        (void)ipc_endpoint_destroy(ep);
        return false;
    }

    /* Before it can run, which is the ordering that has bitten this codebase
     * four separate times: a process that starts before its capabilities are
     * in place finds an empty table. */
    if (ipc_cap_grant(p->thread, ep) != 0) {
        process_abandon(p);
        (void)ipc_endpoint_destroy(ep);
        return false;
    }

    process_grant_console(p);
    process_start(p);

    /*
     * Let it get as far as the receive and block. Bounded rather than
     * waiting on a state, because "it is blocked" is what the test is about
     * and inferring it from the thread would be checking the kernel's
     * bookkeeping instead of its behaviour.
     */
    for (i = 0; i < 20000 && !p->exited; i++) {
        thread_yield();
    }

    if (p->exited) {
        /* It came back before anything destroyed the endpoint, so whatever
         * it returned says nothing about the wake-up. */
        process_reap(p);
        (void)ipc_endpoint_destroy(ep);
        return false;
    }

    (void)ipc_endpoint_destroy(ep);

    for (i = 0; i < 20000 && !p->exited; i++) {
        thread_yield();
    }

    if (!p->exited) {
        return false;
    }

    code = p->exit_code;
    process_reap(p);

    return code == 0 && process_count() == before;
}

/*
 * M6: the display.
 *
 * `hal_fb_init` is idempotent - it rewrites the same config for the same
 * static buffer - so a test may call it to get the descriptor. The suite
 * runs with `-device ramfb`, deliberately: without it these would all pass
 * by being skipped, which is the failure mode a test about a device most
 * easily hides behind.
 */
static bool fb_get(struct fb *out)
{
    return hal_fb_init(out);
}

/*
 * The console's colour, and UTF-8 with it.
 *
 * One assertion covering both, because on this path they are one thing: a
 * write says what colour it is in, and the bytes it carries are decoded
 * before they reach a glyph. Counting pixels of an unlikely colour is what
 * makes it exact rather than plausible.
 *
 * **128 and not "more than before".** U+2588 FULL BLOCK is every pixel of
 * one 8x16 cell, so the right answer is the only answer: three unknown
 * boxes - which is what this console drew for a three-byte character until
 * `screen_byte` decoded them - is a different number, a block found at the
 * wrong index in the font is a different number, and a block drawn in the
 * console's own colour is zero.
 *
 * The third write is the half that a mode would fail. `kputs` has no
 * opinion about colour, so if the previous write had *set* one rather than
 * carried it, this would go on painting in it and the count would climb.
 */
static unsigned long fb_pixels_of(const struct fb *fb, uint32_t want)
{
    unsigned long found = 0;
    unsigned y, x;

    for (y = 0; y < fb->height; y++) {
        const volatile uint32_t *row = (const volatile uint32_t *)
            ((const volatile unsigned char *)fb->pixels
             + (size_t)y * fb->pitch);

        for (x = 0; x < fb->width; x++) {
            if (row[x] == want) {
                found++;
            }
        }
    }

    return found;
}

static bool test_console_colour_and_utf8(void)
{
    /* Nothing else on this screen is drawn in it, and `before` covers the
     * chance that something is. */
    const uint32_t ODD = 0xff123456u;
    const unsigned long CELL = 8u * 16u;    /* one glyph, in pixels */

    struct fb fb;
    unsigned long before, after, again;
    bool ok;

    if (!fb_get(&fb)) {
        return false;
    }

    before = fb_pixels_of(&fb, ODD);

    kwrite_colour("\xe2\x96\x88", 3, ODD);        /* U+2588, in that colour */
    after = fb_pixels_of(&fb, ODD);

    kputs("\xe2\x96\x88");                        /* the same block, no colour */
    again = fb_pixels_of(&fb, ODD);

    /*
     * A newline before returning, and it is not tidiness.
     *
     * This test draws, which means those two blocks go to the serial line
     * as well - and the runner reads that line for TAP, where a result has
     * to begin a line. Without this the next `ok` was printed on the end of
     * them, did not match, and a run in which every test passed was
     * reported as one test short.
     */
    kputs("\n");

    ok = (after - before == CELL) && (again == after);

    return ok;
}

static bool test_the_display_comes_up(void)
{
    struct fb fb;

    if (!fb_get(&fb)) {
        return false;
    }

    return fb.pixels != NULL && fb.width > 0 && fb.height > 0;
}

static bool test_the_pitch_is_not_the_width(void)
{
    /*
     * The one that has to fail loudly if somebody tidies it away.
     *
     * `gfx.md` §19.3: the pitch is almost never width * 4, and code that
     * assumes it is works perfectly on a display where it happens to be
     * true. This board pads the stride on purpose so that assumption breaks
     * here rather than on the first real hardware, and this test is what
     * stops the padding being removed as an oddity.
     */
    struct fb fb;

    if (!fb_get(&fb)) {
        return false;
    }

    return fb.pitch > fb.width * 4;
}

static bool test_the_framebuffer_is_page_aligned_and_in_ram(void)
{
    /*
     * QEMU is handed the physical address, and a process will one day have
     * this mapped into its own space. Both want pages.
     */
    struct fb fb;
    struct memrange ram;
    uintptr_t base;
    size_t bytes;

    if (!fb_get(&fb)) {
        return false;
    }

    hal_ram_range(&ram);
    base  = (uintptr_t)fb.pixels;
    bytes = (size_t)fb.pitch * fb.height;

    return (base % PAGE_SIZE) == 0
        && base >= ram.base
        && base + bytes <= ram.base + ram.size;
}

static bool test_the_framebuffer_is_writable_end_to_end(void)
{
    /*
     * Every row, at both ends, through the pitch. A mapping that covers the
     * first page and not the last would pass a test that only touched pixel
     * zero, and three megabytes is more than one 2 MB block.
     */
    struct fb fb;
    unsigned y;

    if (!fb_get(&fb)) {
        return false;
    }

    for (y = 0; y < fb.height; y++) {
        volatile uint32_t *row =
            (volatile uint32_t *)((volatile uint8_t *)fb.pixels
                                  + (size_t)y * fb.pitch);

        row[0]            = 0x00abcdefu ^ y;
        row[fb.width - 1] = 0x00fedcbau ^ y;
    }

    for (y = 0; y < fb.height; y++) {
        volatile uint32_t *row =
            (volatile uint32_t *)((volatile uint8_t *)fb.pixels
                                  + (size_t)y * fb.pitch);

        if (row[0] != (0x00abcdefu ^ y) || row[fb.width - 1] != (0x00fedcbau ^ y)) {
            return false;
        }
    }

    return true;
}

static bool test_the_padding_is_real(void)
{
    /*
     * The trap, proven rather than asserted.
     *
     * `test_the_pitch_is_not_the_width` says the numbers differ. This says
     * the difference *matters*: a pixel written where the pitch puts it is
     * not where width * 4 arithmetic would look for it, so code that makes
     * that assumption reads the wrong pixel rather than getting away with
     * it.
     *
     * That is the whole reason this board pads its stride, and this is the
     * test that fails if somebody decides the padding was an oddity and
     * removes it.
     */
    struct fb fb;
    volatile uint8_t *base;
    volatile uint32_t *by_pitch;
    volatile uint32_t *by_width;
    unsigned row = 100;

    if (!fb_get(&fb)) {
        return false;
    }

    base     = (volatile uint8_t *)fb.pixels;
    by_pitch = (volatile uint32_t *)(base + (size_t)row * fb.pitch);
    by_width = (volatile uint32_t *)(base + (size_t)row * fb.width * 4);

    by_pitch[0] = 0x00c0ffeeu;
    by_width[0] = 0x00badcabu;

    /* Two different addresses, so the second write cannot have landed on
     * the first. If the stride were width * 4 they would be the same
     * pointer and this would read back 0x00badcab. */
    return by_pitch[0] == 0x00c0ffeeu;
}

static bool test_enough_address_spaces_for_every_process(void)
{
    /*
     * ADDRSPACE_MAX lives in arch/aarch64/mmu.c and PROCESS_MAX in
     * kernel/process.h, and nothing checks the two against each other at
     * compile time.
     *
     * The reason given here used to be that `arch/` "must not include a
     * kernel header", and that is not true and was not true when it was
     * written: `mmu.c` already includes `pmm.h`, `panic.h` and `hal.h`, all
     * of which are on the `-Ikernel` path, and it includes `spinlock.h` now
     * as well. `arch/` is reimplemented rather than abstracted, which is a
     * statement about *what* it may know, not a rule against knowing how
     * many processes there are.
     *
     * So the honest version is that nobody has made it a compile-time check,
     * and this is the runtime one.
     *
     * So they are checked here, by creating as many spaces as there can be
     * processes. Every process has exactly one, so a shortfall means a spawn
     * that fails for no reason any report can explain - which is precisely
     * what happened: the pools said 16 of 32 processes and 17 of 48 threads
     * with 469 MB free, and the spawn still refused.
     */
    /*
     * **Asked of the pool, not taken from it, and that is the fix.**
     *
     * This used to create PROCESS_MAX spaces and check that none of them
     * refused. That is a stronger statement than the one above it and a
     * different one: taking *all* thirty-two slots only succeeds if nothing
     * anywhere in the machine is holding a single address space at that
     * instant. So the check was two assertions in a trenchcoat - the
     * constants agree, which is what it documents, and the whole machine is
     * idle, which it never said and nobody knew it was making.
     *
     * The second one is not this test's business and is not reliably true.
     * The suite creates and destroys processes before reaching here, several
     * behind waits bounded by a fixed number of yields, and one process that
     * has not finished leaving is one slot short.
     *
     * What was observed: it failed in two runs out of three, having been
     * green for months, and memory was never close - the pool is thirty-two
     * slots against roughly a hundred and twenty-eight thousand free pages,
     * so the margin on pages is four thousandfold and the margin on slots is
     * exactly zero. Why the timing moved is not established; `-smp 4` on the
     * runner is the obvious suspect, because TCG round-robins four vCPUs and
     * the suite runs without `-icount`, but that is a suspicion and is
     * written here as one.
     *
     * `as_total` is the pool's size and `as_count` is how much of it is
     * spoken for; both already exist and are exported for exactly this.
     * Comparing the first against PROCESS_MAX is the assertion the comment
     * above describes, and it cannot be perturbed by anything else running.
     */
    if (as_total() < PROCESS_MAX) {
        return false;
    }

    /*
     * And the property that the pool really hands them out, measured
     * against what is already taken rather than against zero.
     *
     * Kept because "the constants agree" would not have caught a pool that
     * refuses on the last slot, and that is a real failure mode. Asking for
     * everything that is *free* is the same question without the hidden
     * assumption: on an idle machine it is still all thirty-two.
     */
    unsigned baseline = as_count();
    unsigned want = as_total() - baseline;
    struct addrspace *made[PROCESS_MAX];
    unsigned i;
    bool ok = true;

    if (want > PROCESS_MAX) {
        want = PROCESS_MAX;
    }

    for (i = 0; i < want; i++) {
        made[i] = as_create();

        if (made[i] == NULL) {
            ok = false;
            break;
        }
    }

    while (i > 0) {
        as_destroy(made[--i]);
    }

    return ok;
}

static bool test_the_keyboard_came_up(void)
{
    /*
     * The whole virtio bring-up in one assertion: a window found by magic
     * and device id, the modern interface accepted, VIRTIO_F_VERSION_1
     * negotiated and read back, a queue sized, addressed and made ready, and
     * every buffer offered. Any of those going wrong returns false.
     *
     * The suite runs with `-device virtio-keyboard-device` and
     * `-global virtio-mmio.force-legacy=false`, deliberately. Without the
     * second QEMU reports the legacy interface, this refuses it correctly,
     * and the test would fail - which is right: a keyboard nobody can read
     * is not a keyboard.
     *
     * Idempotent, so calling it here after kmain already did is safe.
     */
    return hal_keyboard_init();
}

static bool test_the_boot_announced_every_stage(void)
{
    /*
     * BOOT_STAGES is a constant and the calls to boot_stage are scattered
     * through kmain, so the two can drift - and the only symptom is a
     * progress bar that stops at four fifths and stays there, which reads as
     * "something hung" rather than as "somebody added a stage".
     *
     * The suite runs after the last of them, so by now the two must agree
     * exactly. Too few means a stage was added without changing the
     * constant; too many means one was removed.
     */
    return boot_stages_done() == BOOT_STAGES;
}

static bool test_the_page_allocator_never_hands_out_the_framebuffer(void)
{
    /*
     * The framebuffer lives inside __image_end, so the allocator counts its
     * pages as the kernel's. If that ever stopped being true, a process
     * would be handed memory QEMU is scanning out, and the symptom would be
     * garbage on screen rather than anything that looks like a bug in the
     * allocator.
     */
    struct fb fb;
    uintptr_t base;
    size_t bytes;
    unsigned i;

    if (!fb_get(&fb)) {
        return false;
    }

    base  = (uintptr_t)fb.pixels;
    bytes = (size_t)fb.pitch * fb.height;

    for (i = 0; i < 64; i++) {
        void *p = pmm_alloc_page();
        uintptr_t a = (uintptr_t)p;

        if (p == NULL) {
            return false;
        }

        if (a >= base && a < base + bytes) {
            pmm_free_page(p);
            return false;
        }

        pmm_free_page(p);
    }

    return true;
}

/*
 * M1: the interrupt controller and the timer.
 */
static bool test_irqs_are_unmasked(void)
{
    /* Masked at entry on both boards - `start.S` enters EL1 with DAIF set,
     * and the x86 stub starts with IF clear - and cleared by kmain once
     * there is a handler and a source. `test_interrupts_enabled` is where
     * the two spellings of "masked" are reconciled, one of them being the
     * inverse of the other. */
    return test_interrupts_enabled();
}

static bool test_timer_ticks_advance(void)
{
    /*
     * Bounded by the counter rather than by a loop count, so a dead timer
     * fails in a tenth of a second instead of hanging until the host gives
     * up. Ten periods is a generous window for one interrupt.
     */
    unsigned long start = hal_ticks();
    uint64_t begin = cntpct();
    uint64_t limit = cntfrq() / 10;

    while (cntpct() - begin < limit) {
        if (hal_ticks() != start) {
            return true;
        }
    }

    return false;
}

/*
 * The tick count and the counter, read as one.
 *
 * Reading them as two statements is not enough: preemption can land between
 * them, and then the two numbers describe moments that are a scheduling
 * quantum apart. Masking for the two reads makes the pair atomic without
 * masking for the measurement, which would stop the very ticks being
 * counted.
 */
static void sample_clock(unsigned long *ticks, uint64_t *counter,
                         unsigned long *missed)
{
    uint64_t saved = cpu_interrupts_save();

    *ticks = hal_ticks();
    *counter = cntpct();
    *missed = hal_ticks_missed();

    cpu_interrupts_restore(saved);
}

static bool test_timer_period_matches_the_rate(void)
{
    /*
     * The test that would have caught the drift.
     *
     * Rearming the timer from "now" rather than from the previous deadline
     * folds the cost of taking the interrupt into every period. Nothing else
     * here notices: ticks still advance, the count is still exact, and a
     * nominal 100 Hz quietly runs at 73. Only measuring the counter across
     * several periods shows it.
     *
     * Ten percent, and the number is measured rather than picked. Across
     * twenty runs the observed error is 0 to 11 parts per thousand, and it
     * stays under 5 with the host deliberately loaded. Five percent was the
     * first threshold and it produced one false failure during a parallel
     * rebuild, which is the worst kind: a test that is red occasionally
     * teaches you to stop reading it.
     *
     * Ten percent keeps roughly a tenfold margin over what is actually
     * observed while still catching the bug this exists for, which was a
     * 27 percent drift.
     */
    unsigned attempt;

    /*
     * Measured only across a window where no deadline was missed.
     *
     * A missed deadline is a tick that never happened, so the tick count and
     * elapsed time stop agreeing, and a measurement across such a window is
     * measuring the load rather than the timer. That is not a flaw to widen
     * the tolerance around: it is a different quantity.
     *
     * This showed up as roughly one run in fifteen coming out 6 to 10
     * percent long, which is far outside anything the timer itself does and
     * exactly what dropping one tick in a hundred looks like.
     */
    for (attempt = 0; attempt < 8; attempt++) {
        unsigned long k0, k1, m0, m1;
        uint64_t t0, t1, elapsed, expected, tolerance;

        /* Start on a tick boundary so the first period is a whole one. */
        k0 = hal_ticks();
        while (hal_ticks() == k0) { }

        sample_clock(&k0, &t0, &m0);

        while (hal_ticks() - k0 < 100) { }

        sample_clock(&k1, &t1, &m1);

        if (m1 != m0 || k1 <= k0) {
            continue;       /* the system fell behind; this window says nothing */
        }

        elapsed = t1 - t0;

        /*
         * Divided by the ticks that actually elapsed rather than by the
         * number this loop waited for, so being descheduled inside the loop
         * costs accuracy nowhere.
         *
         * Ten percent, and the number is measured rather than picked. On a
         * clean window the observed error is 0 to 2 parts per thousand. It
         * exists to catch the bug it was written for, which was a 27 percent
         * drift from rearming the timer relative to now instead of relative
         * to the previous deadline.
         */
        expected  = (cntfrq() / TICK_HZ) * (k1 - k0);
        tolerance = expected / 10;

        return elapsed > expected - tolerance
            && elapsed < expected + tolerance;
    }

    /* Eight windows in a row with a missed deadline is not a measurement
     * problem, it is the system failing to keep up with a 100 Hz timer. */
    return false;
}



/*
 * M2: the freestanding libc.
 */
static bool test_memcpy_and_memcmp(void)
{
    char src[16] = "0123456789abcde";
    char dst[16];

    memset(dst, 0x5a, sizeof(dst));
    memcpy(dst, src, sizeof(src));

    return memcmp(dst, src, sizeof(src)) == 0;
}

static bool test_memset_fills_exactly(void)
{
    /* Guards on both sides, because the failure that matters is writing one
     * byte too many, and a fill that is correct in the middle looks correct. */
    unsigned char buf[16];

    memset(buf, 0x00, sizeof(buf));
    memset(buf + 4, 0xab, 8);

    return buf[3] == 0x00 && buf[4] == 0xab
        && buf[11] == 0xab && buf[12] == 0x00;
}

static bool test_memmove_handles_overlap(void)
{
    /*
     * The whole reason memmove exists. Shifting up by one overlaps, and a
     * forward copy reads bytes it has already written, smearing the first
     * byte across the buffer. memcpy is allowed to do exactly that.
     */
    char buf[8] = { 1, 2, 3, 4, 5, 6, 7, 0 };

    memmove(buf + 1, buf, 6);

    return buf[0] == 1 && buf[1] == 1 && buf[2] == 2
        && buf[3] == 3 && buf[6] == 6;
}

static bool test_string_functions(void)
{
    const char *s = "kosmos";

    return strlen("") == 0
        && strlen(s) == 6
        && strcmp("a", "a") == 0
        && strcmp("a", "b") < 0
        && strcmp("b", "a") > 0
        && strncmp("abcd", "abce", 3) == 0
        && strncmp("abcd", "abce", 4) < 0
        && strchr(s, 'm') == s + 3   /* k o s m */
        && strchr(s, 'z') == NULL
        && strchr(s, '\0') == s + 6;   /* the terminator is findable */
}

/*
 * Does a preemption preserve the whole FP register file?
 *
 * `context_switch` saves d8 to d15, which is the callee-saved set and the
 * right answer for a thread that *called* it: the compiler already spilled
 * everything else. A preempted thread did not call anything. It was
 * interrupted between two instructions, possibly with a live d0, and the
 * exception entry saves no FP state at all - so if the thread scheduled
 * next uses d0, the first thread comes back with the second one's value.
 *
 * Written as two threads that each hold a known value in d0 across a spin
 * long enough to be preempted, because a statistical version of this in Lua
 * proves nothing: Lua keeps its locals on its own stack in memory, so the
 * window where a double is live in a hardware register is a few
 * instructions per opcode and a test that misses it looks like a pass.
 *
 * The spin is inside the asm block on purpose. A loop in C around it would
 * be a call boundary, where the ABI says d0 is dead anyway and the bug
 * cannot show.
 */
static volatile unsigned fp_preempt_checks;
static volatile unsigned fp_preempt_wrong;
static volatile unsigned fp_preempt_switches;

static void fp_spinner(void *arg)
{
    unsigned long want = (unsigned long)(uintptr_t)arg;
    unsigned last = thread_current()->switches;
    unsigned i;

    for (i = 0; i < 240u; i++) {
        unsigned long got;

        /* Two nested loops, so one spin is longer than a timer period.
         * A shorter one finishes inside a quantum, is never preempted, and
         * passes without testing anything - which is what the first version
         * of this did, and the switch counter below is here so it cannot
         * happen again quietly. */
        TEST_FP_SPIN(got, want);

        {
            /* Summed over both threads rather than assigned, which is what
             * the first version did - and with two threads writing one slot
             * it reported whichever finished last instead of the total. */
            unsigned now = thread_current()->switches;

            fp_preempt_switches += now - last;
            last = now;
        }

        fp_preempt_checks++;

        if (got != want) {
            fp_preempt_wrong++;
        }
    }
}

/*
 * Priorities, and the two things they have to do.
 *
 * `sched_rr.c` said in its first comment that there was "nothing to
 * prioritise", while `design.md` and `ui.md` both called input at the
 * highest priority non-negotiable. This is that sentence becoming code, and
 * these are the two properties it has to have.
 */
static volatile unsigned prio_order[8];
static volatile unsigned prio_count;

static void prio_marker(void *arg)
{
    unsigned me = (unsigned)(uintptr_t)arg;

    if (prio_count < 8) {
        prio_order[prio_count++] = me;
    }

    thread_exit();
}

static bool test_higher_priority_runs_first(void)
{
    struct thread *low, *high;
    unsigned i;

    prio_count = 0;

    /* Enqueued low first, so running it first is what a FIFO would do and
     * what a priority queue must not. */
    low = thread_create_suspended("prio-low", prio_marker, (void *)1);
    high = thread_create_suspended("prio-high", prio_marker, (void *)2);

    if (low == NULL || high == NULL) {
        return false;
    }

    /*
     * The "low" one is at NORMAL, not LOW - the same band as the thread
     * running this test, so round robin gives it a turn. At LOW it would
     * never run at all while this test is spinning at NORMAL, and the test
     * would be measuring starvation rather than ordering. That is correct
     * behaviour for strict priority and a badly built test.
     */
    thread_set_priority(low, SCHED_PRIO_NORMAL);
    thread_set_priority(high, SCHED_PRIO_DISPLAY);

    thread_wake(low);
    thread_wake(high);

    for (i = 0; i < 100000u && prio_count < 2; i++) {
        thread_yield();
    }

    /* The one enqueued second, and higher, ran first. */
    return prio_count == 2 && prio_order[0] == 2 && prio_order[1] == 1;
}

/*
 * Does anything at NORMAL run while the desktop is up?
 *
 * The question is not academic and it is not about a spinner outranking
 * something. `test_higher_priority_runs_first` already notes that a lower
 * band never runs while a higher one *spins*, and calls that correct for
 * strict priority. This asks the other case: the higher band is a
 * compositor, which is not spinning at all - it wakes on a timer, does a
 * few microseconds of work, and blocks again for the rest of the tick.
 *
 * That is what `wm` measurably does: 267 ms of work in a six second run on
 * an idle desktop, 4.5% of the machine. So the ninety-five per cent it is
 * not using has to be available to whatever is below it, and a thread at
 * NORMAL that yields in a loop has to make progress.
 *
 * It did not, on the real machine: with programs at NORMAL instead of
 * being promoted into the compositor's band, `say 3 hello` never reached a
 * *wall clock* deadline it would have reached on one per cent of a core.
 * This is that, reduced to two threads and no desktop.
 */
static volatile unsigned long starve_spins;
static volatile bool          starve_stop;
static volatile bool          starve_done;

static void starve_sleeper(void *arg)
{
    (void)arg;

    /*
     * The shape of a compositor's loop: wake, do almost nothing, sleep
     * again for the rest of the tick.
     *
     * `thread_deadline_in`, not `hal_ticks() + 1`. Deadlines are counter
     * values now - `thread.h` says why - and a tick count is a number so
     * small that it is always in the past, so this thread stopped sleeping
     * and became a spinner at DISPLAY priority. It starved the NORMAL
     * thread it was supposed to be sharing with, which is precisely what
     * this test exists to notice, and it noticed.
     */
    while (!starve_stop) {
        thread_sleep_until(thread_deadline_in(1));
    }

    thread_exit();
}

static void starve_spinner(void *arg)
{
    unsigned long deadline = hal_ticks() + (unsigned long)(uintptr_t)arg;

    /*
     * Bounded by the *clock*, not by a count. That is the whole point: the
     * loop finishes after so many ticks of wall time however little of the
     * processor it gets, so a test that never finishes means it got none.
     */
    while (hal_ticks() < deadline) {
        starve_spins++;
        thread_yield();
    }

    starve_done = true;
    thread_exit();
}

/*
 * A sleep lasts as long as it asked for.
 *
 * **Nothing checked this, on either board, and that is how x86 shipped
 * with every timeout in the system lasting four milliseconds.** The suite
 * had tests for the scheduler picking the right thread, for preemption, for
 * a wake outranking the running thread - and none for the plainest property
 * of all, which is that `sleep(n)` takes n.
 *
 * It went unseen because nothing *else* checks a duration either: the
 * display harness times its own waits from the host and tolerates seconds,
 * `latency` measures a yield rather than a sleep, and every server that
 * cares simply appeared to be fast. A sleep that returns early looks like a
 * quick machine.
 *
 * Measured in the counter, because the counter is the clock that does not
 * depend on the thing being tested. Bounded below and not above at the
 * tight end: `thread_wake_sleepers` only ever wakes a thread whose deadline
 * has *arrived*, so a sleep can be late and must never be early - which
 * makes "at least what was asked" an exact assertion rather than a
 * tolerance. The upper bound is loose because a woken thread still has to
 * be scheduled.
 */
#define SLEEP_TEST_TICKS   25u          /* 100 ms at TICK_HZ 250 */

static volatile uint64_t sleep_measured;
static volatile bool     sleep_finished;

static void sleep_measurer(void *arg)
{
    uint64_t began = cpu_cycles();

    (void)arg;

    thread_sleep_until(thread_deadline_in(SLEEP_TEST_TICKS));

    sleep_measured = cpu_cycles() - began;
    sleep_finished = true;
    thread_exit();
}

/*
 * Per-CPU state is per *CPU*, not per thread.
 *
 * **This is the distinction the x86-64 port got wrong the expensive way**,
 * and it is worth a test before there is a second core rather than after.
 * `user_rsp` was a global holding the interrupted stack pointer. It looked
 * like per-CPU state - one core, one interrupted stack - and it was
 * per-*thread*: a process that blocked in IPC let another run, the second
 * overwrote the first's, and a process returned to user level on somebody
 * else's stack. One core was enough to prove it wrong.
 *
 * So: two threads, and they must see the *same* `this_cpu`. On one core
 * that is the whole assertion; on two it becomes "the same as long as they
 * are on the same core", which is a different test written when there is a
 * second core to write it against.
 *
 * What it catches today is the mechanism itself. If `cpu_set_self` never
 * ran, or `TPIDR_EL1` is not banked the way it is believed to be, or the
 * compiler folded `this_cpu()` into something per-call, this says so - and
 * it says so on both boards, where one uses a register and the other a
 * static that is honest about being one.
 */
static volatile struct percpu *seen_by_other;
static volatile bool           other_looked;

static void percpu_looker(void *arg)
{
    (void)arg;

    seen_by_other = this_cpu();
    other_looked  = true;
    thread_exit();
}

/*
 * The machine has processors, and the kernel says how many of them it uses.
 *
 * **Two numbers that must not be one.** `hal_cpu_count` asks the firmware
 * what the machine has; `thread_cpu_count` is `NR_CPUS`, what this kernel
 * was built for. On `-smp 4` the first is four and the second is one, and a
 * system that reported only the second would look finished on a machine it
 * is using a quarter of.
 *
 * Asserted as a *relationship* rather than as numbers, because the harness
 * runs this on four and a person runs it on whatever they have: a machine
 * cannot have fewer processors than the kernel is scheduling on, and the
 * kernel cannot be scheduling on none.
 *
 * What it catches is the discovery breaking. On AArch64 that is a PSCI walk
 * - ask about processor 0, 1, 2 until the firmware says there is no such
 * thing - and a walk that returned zero, or ran off the end, would show
 * here rather than in a boot line nobody reads.
 */
static bool test_the_machine_says_how_many_processors_it_has(void)
{
    unsigned present = hal_cpu_count();
    unsigned in_use  = thread_cpu_count();

    return in_use >= 1 && present >= in_use && present <= 64;
}

/*
 * Every processor that entered the kernel claimed *its own* slot.
 *
 * **This is what step one could not check and step three can.** The per-CPU
 * pointer lives in `TPIDR_EL1`, which the architecture says is banked per
 * core - and on a machine with one core that claim is unfalsifiable,
 * because every answer is the same answer whether the register works or
 * not.
 *
 * With secondaries running there is a real test: each was started with its
 * own index and called `percpu_init` with it, so `cpus[n].index` is `n` for
 * every core that arrived. A register that were somehow shared, or an entry
 * path that passed the wrong context word, would leave those slots at the
 * zero `.bss` gave them.
 *
 * It also checks the three counts stay in their order, which is the thing
 * that went wrong twice while this was being written: `hal_cpu_count` is
 * what the machine has, `smp_online` is how many ran kernel code, and
 * `thread_cpu_count` is how many schedule threads. Reporting `NR_CPUS` for
 * the last said "4 scheduling" on a machine with three cores in `wfi`.
 */
static bool test_every_processor_claimed_its_own_slot(void)
{
    unsigned online  = smp_online();
    unsigned present = hal_cpu_count();
    unsigned i;

    if (online < 1 || online > present) {
        return false;
    }

    if (thread_cpu_count() > online) {
        return false;           /* scheduling on cores that never arrived */
    }

    for (i = 0; i < online; i++) {
        const struct percpu *c = percpu_at(i);

        if (c == NULL || c->index != i) {
            return false;
        }
    }

    return true;
}

/*
 * Every processor that arrived is taking its own timer interrupt.
 *
 * **The first check in this project that can tell a live secondary from a
 * dead one.** Until now a core that started, claimed its slot and then
 * wedged looked exactly like one parked in `wfi`: `smp_online` counts cores
 * that reached `secondary_main`, and nothing afterwards ever asked whether
 * they were still there. The boot log said "ticking and idle" on the
 * strength of having called the function.
 *
 * The generic timer is per-PE by architecture - each core arms its own
 * CNTP_CVAL_EL0 and takes its own PPI 30 - so a secondary's tick count
 * rising is proof that it woke, took an exception through a vector table it
 * installed itself, found its own redistributor, and rearmed. Four separate
 * things, and one counter shows all of them.
 *
 * Bounded by this core's own clock rather than by a count of yields, which
 * is the lesson `as: one space per possible process` cost: a fixed number of
 * iterations is a duration only until the machine's timing changes.
 *
 * Half of core zero's ticks is the threshold, and it is deliberately loose.
 * The cores are not synchronised, this test starts at an arbitrary point in
 * each one's period, and TCG round-robins four vCPUs - so the honest
 * assertion is "it is ticking", not "it is ticking at the same rate".
 */
/*
 * The lock itself: it excludes, it says who holds it, and it masks.
 *
 * **A lock is the one thing in a kernel that cannot be tested by using it.**
 * Every structure it protects works perfectly well with a lock that does
 * nothing at all, on a machine where nothing contends - which is this
 * machine today, and is exactly why `docs/smp.md` step two called an
 * uncontended lock untestable and skipped it. So this tests the mechanism
 * rather than the protection: the three properties a caller relies on, each
 * of which is checkable on one core.
 *
 * What it cannot check here is mutual exclusion under real contention. That
 * needs two cores inside the same critical section, which is what `stress`
 * asks for once a secondary runs threads.
 */
static bool test_a_spinlock_excludes_and_masks(void)
{
    static struct spinlock probe = SPINLOCK("test-probe");
    unsigned long flags;

    if (probe.locked != 0u || probe.holder != SPIN_NOBODY) {
        return false;           /* not free before anyone took it */
    }

    if (spin_held_here(&probe)) {
        return false;
    }

    flags = spin_lock(&probe);

    /* Held, by this processor, and it says so. */
    if (probe.locked == 0u || !spin_held_here(&probe)) {
        spin_unlock(&probe, flags);
        return false;
    }

    if (probe.holder != this_cpu()->index) {
        spin_unlock(&probe, flags);
        return false;
    }

    /*
     * And interrupts are off. This is the property the whole design rests
     * on - a lock reached from both a syscall and an interrupt handler is a
     * self-deadlock the moment one is taken with interrupts enabled - and
     * it is invisible from anywhere else.
     */
    if (cpu_interrupts_enabled()) {
        spin_unlock(&probe, flags);
        return false;
    }

    /*
     * A second attempt from this same core must fail rather than succeed.
     * `cpu_lock_try` is what a contending core calls, so this is the closest
     * a single processor can get to standing in for one.
     */
    if (cpu_lock_try(&probe.locked)) {
        return false;           /* handed out twice; do not unlock */
    }

    spin_unlock(&probe, flags);

    if (probe.locked != 0u || probe.holder != SPIN_NOBODY) {
        return false;
    }

    /* And the interrupt state came back as it was, not merely enabled. */
    return true;
}

static bool test_every_processor_takes_its_own_ticks(void)
{
    unsigned online = smp_online();
    unsigned long before[NR_CPUS];
    unsigned long start;
    unsigned i;

    if (online <= 1) {
        return true;            /* one processor; nothing to compare against */
    }

    for (i = 0; i < online && i < NR_CPUS; i++) {
        before[i] = hal_ticks_on(i);
    }

    start = hal_ticks();

    /*
     * Wait on this core's own tick count, which is the clock this test is
     * allowed to trust. `cpu_wait_for_interrupt` rather than a spin so the
     * wait costs nothing and cannot be optimised into one.
     */
    while (hal_ticks() - start < 50UL) {
        cpu_wait_for_interrupt();
    }

    for (i = 1; i < online && i < NR_CPUS; i++) {
        if (hal_ticks_on(i) - before[i] < 25UL) {
            return false;
        }
    }

    return true;
}

/*
 * Every processor is idling as a thread, and charging the time to itself.
 *
 * `docs/smp.md` step four is "the idle thread on the second core: it
 * schedules, ticks and idles", and this is the half the tick test above
 * cannot see. A core can take timer interrupts with `current` still NULL -
 * that is exactly what step three's cores did - and `thread_tick` then
 * returns before charging anything, so the core is alive and invisible.
 *
 * Three things at once, and each has failed on its own during this step:
 * that a secondary has a `current` at all; that it is *its own* idle thread
 * rather than core zero's, which is what a shared global would have given;
 * and that the time is landing in that core's counters rather than core
 * zero's, which is what `thread_tick` charging `this_cpu()` buys.
 *
 * Idle and not busy, deliberately. A secondary has no runqueue, so if it is
 * ever charged busy time something has scheduled work onto a core that
 * cannot run it.
 */
/*
 * A thread that runs on another processor, and reports which one.
 *
 * **This is the whole of `docs/smp.md` steps five and six on one thread.**
 * Everything a cross-core thread depends on has to work for this to pass,
 * and each of them fails differently:
 *
 *   - placement, or the thread comes home to this core and the check that
 *     it ran *elsewhere* fails;
 *   - the target core's runqueue and its lock, or the enqueue lands
 *     nowhere;
 *   - the IPI, or it is not lost but *late* - which is why this waits on
 *     the clock and would still pass without one, so the timing is measured
 *     rather than asserted below;
 *   - the target's idle loop, which has to notice a queue that was empty a
 *     moment ago;
 *   - and `thread_block`'s idle fallback, because the thread exits and
 *     leaves that core with nothing to run, which used to be a panic.
 *
 * It records `this_cpu()->index` from inside the thread, which is the only
 * evidence that cannot be faked by the thread having run here.
 */
static volatile unsigned elsewhere_ran_on = (unsigned)-1;
static volatile bool     elsewhere_done;

static void elsewhere_thread(void *arg)
{
    (void)arg;

    elsewhere_ran_on = this_cpu()->index;
    elsewhere_done = true;
}

static bool test_a_thread_runs_on_another_processor(void)
{
    unsigned long start;
    unsigned      target = 1;

    if (smp_online() <= 1) {
        return true;            /* one processor; nothing to run elsewhere */
    }

    elsewhere_ran_on = (unsigned)-1;
    elsewhere_done = false;

    if (thread_create_on(target, "elsewhere", elsewhere_thread, NULL) == NULL) {
        return false;
    }

    /*
     * Waited out on this core's clock, and **this core never yields**.
     *
     * That is the point rather than an oversight: if the thread only ran
     * because this processor gave up the CPU, it would prove nothing about
     * the other one. Core zero sits here doing nothing at all, and the work
     * still has to get done.
     */
    start = hal_ticks();

    while (!elsewhere_done && hal_ticks() - start < 250UL) {
        cpu_relax();
    }

    if (!elsewhere_done) {
        return false;           /* never ran anywhere */
    }

    return elsewhere_ran_on == target;
}

static bool test_every_processor_idles_as_a_thread(void)
{
    unsigned online = smp_online();
    unsigned long before[NR_CPUS];
    unsigned long start;
    unsigned i;

    if (online <= 1) {
        return true;
    }

    for (i = 1; i < online && i < NR_CPUS; i++) {
        const struct percpu *c = percpu_at(i);

        if (c == NULL || c->idle_thread == NULL) {
            return false;
        }

        if (c->current != c->idle_thread) {
            return false;       /* not adopted, or adopted somebody else's */
        }

        if (c->idle_thread == percpu_at(0)->idle_thread) {
            return false;       /* one idle thread shared by two processors */
        }

        before[i] = c->idle_ticks;
    }

    start = hal_ticks();

    while (hal_ticks() - start < 50UL) {
        cpu_wait_for_interrupt();
    }

    for (i = 1; i < online && i < NR_CPUS; i++) {
        const struct percpu *c = percpu_at(i);

        if (c->idle_ticks - before[i] < 25UL) {
            return false;       /* ticking, but charging it nowhere */
        }
    }

    return true;
}

static bool test_percpu_is_per_cpu_and_not_per_thread(void)
{
    struct percpu *mine = this_cpu();
    struct thread *t;
    unsigned long  guard;

    if (mine == NULL || mine->index != 0) {
        return false;               /* percpu_init never ran */
    }

    /* And it is the same one the running thread is recorded in. */
    if (mine->current != thread_current()) {
        return false;
    }

    seen_by_other = NULL;
    other_looked  = false;

    t = thread_create_suspended("percpu-look", percpu_looker, NULL);

    if (t == NULL) {
        return false;
    }

    thread_wake(t);

    guard = hal_ticks() + 50;

    while (!other_looked && hal_ticks() < guard) {
        thread_yield();
    }

    return other_looked && seen_by_other == mine;
}

static bool test_a_sleep_lasts_as_long_as_it_asked(void)
{
    struct thread *t;
    unsigned long  guard;
    uint64_t       want;

    sleep_measured = 0;
    sleep_finished = false;

    t = thread_create_suspended("sleep-measure", sleep_measurer, NULL);

    if (t == NULL) {
        return false;
    }

    thread_wake(t);

    /*
     * This thread stays runnable while the other sleeps, which is not
     * incidental: `thread_block` panics when nothing else can run, and
     * during the suite there is no idle thread to fall back on.
     */
    guard = hal_ticks() + SLEEP_TEST_TICKS * 8u;

    while (!sleep_finished && hal_ticks() < guard) {
        thread_yield();
    }

    if (!sleep_finished) {
        return false;
    }

    want = test_counter_hz() * SLEEP_TEST_TICKS / TICK_HZ;

    /* Never early, and not absurdly late. The defect this exists for
     * returned in about one tick against twenty-five asked for, so the
     * lower bound is what catches it. */
    return sleep_measured >= want && sleep_measured <= want * 5u;
}

/*
 * And that input does not wake a thread that was not waiting for it.
 *
 * **This is the test the one above could not be.** A sleep of the right
 * length is a property worth asserting and it did not catch the defect it
 * was written for: the trigger is `thread_wake_sleepers_now`, which only
 * runs when an input interrupt has arrived, and in the test image nothing
 * ever sets that flag. Put the bug back and the duration test still passed
 * - which `check_latency` in `run_screenshot.py` already says is worse than
 * having no test at all.
 *
 * So this calls the path directly rather than waiting for a keyboard.
 * `thread_wake_sleepers_now` *is* what an arriving key does, and calling it
 * with a plain sleeper blocked is the whole defect in three lines,
 * deterministic and on both boards.
 *
 * What it was: that function woke every thread carrying a `wake_at` on the
 * grounds that a deadline meant "waiting". A `sys.sleep` carries one and is
 * waiting for nothing. On x86 `input_arrived` latches true and is cleared
 * only by `SYS_WAIT_INPUT`, which nothing calls at a bare prompt, so every
 * timer tick ran this - 1200 times in five seconds, against 0 on AArch64 -
 * and every sleep in the system lasted one tick.
 */
static volatile bool     plain_sleeper_done;
static volatile uint64_t plain_sleeper_slept;

static void plain_sleeper(void *arg)
{
    uint64_t began = cpu_cycles();

    (void)arg;

    thread_sleep_until(thread_deadline_in(SLEEP_TEST_TICKS));

    plain_sleeper_slept = cpu_cycles() - began;
    plain_sleeper_done  = true;
    thread_exit();
}

static bool test_input_does_not_wake_a_plain_sleeper(void)
{
    struct thread *t;
    unsigned long  guard;
    unsigned       i;

    plain_sleeper_done  = false;
    plain_sleeper_slept = 0;

    t = thread_create_suspended("plain-sleep", plain_sleeper, NULL);

    if (t == NULL) {
        return false;
    }

    thread_wake(t);

    /* Let it reach the block. Until it does there is nothing to wake. */
    for (i = 0; i < 4; i++) {
        thread_yield();
    }

    if (plain_sleeper_done) {
        return false;           /* it never slept at all */
    }

    /* A key arrives. This is exactly what the interrupt path does. */
    thread_wake_sleepers_now();

    /* And it is given every chance to run if it was woken. */
    for (i = 0; i < 4; i++) {
        thread_yield();
    }

    if (plain_sleeper_done) {
        return false;           /* woken by input it never asked for */
    }

    /* And it still finishes on its own deadline afterwards, so the
     * assertion above is about *when* it woke and not about a thread that
     * was broken into never waking at all. */
    guard = hal_ticks() + SLEEP_TEST_TICKS * 8u;

    while (!plain_sleeper_done && hal_ticks() < guard) {
        thread_yield();
    }

    return plain_sleeper_done
        && plain_sleeper_slept >= test_counter_hz() * SLEEP_TEST_TICKS
                                  / TICK_HZ;
}

static bool test_normal_runs_while_a_higher_band_sleeps(void)
{
    struct thread *sleeper, *spinner;
    unsigned long  guard;
    bool           finished;

    starve_spins = 0;
    starve_stop  = false;
    starve_done  = false;

    sleeper = thread_create_suspended("starve-sleep", starve_sleeper, NULL);
    spinner = thread_create_suspended("starve-spin", starve_spinner,
                                      (void *)(uintptr_t)10u);

    if (sleeper == NULL || spinner == NULL) {
        return false;
    }

    thread_set_priority(sleeper, SCHED_PRIO_DISPLAY);
    thread_set_priority(spinner, SCHED_PRIO_NORMAL);

    /*
     * And this thread drops to IDLE for the duration, standing in for the
     * idle thread the test image does not have. At NORMAL it would be
     * competing with the thread it is measuring; at IDLE it runs only when
     * genuinely nothing else wants the processor, which is the condition
     * being tested.
     */
    thread_set_priority(thread_current(), SCHED_PRIO_IDLE);

    thread_wake(sleeper);
    thread_wake(spinner);

    /* Generous: the spinner asks for ten ticks and gets fifty before this
     * gives up, so a slow host fails nothing. */
    guard = hal_ticks() + 50;

    while (!starve_done && hal_ticks() < guard) {
        thread_yield();
    }


    finished    = starve_done;
    starve_stop = true;

    for (guard = hal_ticks() + 4; hal_ticks() < guard; ) {
        thread_yield();
    }

    thread_set_priority(thread_current(), SCHED_PRIO_NORMAL);

    /*
     * Two things, and the first is the one that was broken: the spinner
     * reached its own deadline at all. The count is a sanity check on top -
     * a hundred is far below what a working scheduler gives and far above
     * the zero a starved thread gives.
     */
    return finished && starve_spins > 100;
}

static volatile bool preempt_woke;

static void preempt_waker(void *arg)
{
    (void)arg;

    preempt_woke = true;
    thread_exit();
}

/*
 * And that a wake is acted on rather than queued.
 *
 * Without `preempts`, a thread that becomes ready waits for the running
 * thread's quantum - up to a hundred milliseconds at ten ticks. The test is
 * that the higher-priority thread runs *before* the spinner has burned
 * through its turn, which a fair queue could not manage.
 */
static bool test_a_wake_preempts_a_lower_priority_thread(void)
{
    struct thread *high;
    unsigned spins;

    preempt_woke = false;

    high = thread_create_suspended("preempt-high", preempt_waker, NULL);

    if (high == NULL) {
        return false;
    }

    thread_set_priority(high, SCHED_PRIO_INPUT);
    thread_wake(high);

    /*
     * Spin without yielding. If preemption on becoming ready works, the
     * wake hands the CPU over at the next exception; if it does
     * not, nothing runs until this loop gives up.
     *
     * The bound is generous and the assertion is not: what matters is that
     * it happened at all without a yield in this loop.
     */
    for (spins = 0; spins < 20000000u && !preempt_woke; spins++) {
        __asm__ volatile("" ::: "memory");   /* portable: a compiler barrier */
    }

    return preempt_woke;
}

static bool test_fp_survives_a_preemption(void)
{
    unsigned i;

    fp_preempt_checks = 0;
    fp_preempt_wrong  = 0;

    /* Two different bit patterns, so a thread that comes back with the
     * other one's value is unmistakable rather than merely unlikely. */
    if (thread_create("fp-a", fp_spinner, (void *)0x3ff0000000000000UL) == NULL
     || thread_create("fp-b", fp_spinner, (void *)0x4000000000000000UL) == NULL) {
        return false;
    }

    for (i = 0; i < 200000u && fp_preempt_checks < 480u; i++) {
        thread_yield();
    }

    return fp_preempt_checks >= 480u && fp_preempt_switches >= 4u
           && fp_preempt_wrong == 0;
}

/* Lazy FP save, from arch/aarch64/fp.c. */
bool fp_owned_by(const struct thread *t);
void fp_reset(void);

/*
 * That the kernel may use floating point at all.
 *
 * On both boards this is a thing the kernel had to *arrange*, and the two
 * arrangements have nothing in common. AArch64 resets CPACR_EL1.FPEN to
 * "trap everything", so the first `setjmp` - which saves d8 to d15 - was an
 * EC 0x07 trap whose name says nothing about floating point unless you go
 * looking. That is how it was found. x86 raises #UD on any SSE instruction
 * until CR4.OSFXSR says the operating system knows how to save the state,
 * and emulates every FP instruction into a trap nothing handles unless
 * CR0.EM is clear.
 *
 * What this asserts is the outcome the two share: arithmetic works. Not the
 * mechanism, because the mechanisms are different and one of them changed
 * once already - this used to assert `FPEN == 0b11` and stopped being true
 * the day the save became lazy.
 *
 * The arithmetic is in assembly because both kernels are built without FP -
 * `-mgeneral-regs-only` on one, `-mno-sse` on the other - and neither
 * compiler will emit an instruction for it.
 */
static bool test_fp_is_usable_in_the_kernel(void)
{
    uint64_t bits;

#if defined(__aarch64__)
    __asm__ volatile(
        "fmov d0, #1.0\n"
        "fadd d0, d0, d0\n"
        "fmov %0, d0\n"
        : "=r"(bits) : : "d0");
#else
    /* No immediate form here: 1.0 comes in as a bit pattern and goes
     * through a general register, which is what `movq` between the two
     * files is for. */
    uint64_t one = 0x3ff0000000000000UL;

    __asm__ volatile(
        "movq %1, %%xmm0\n"
        "addsd %%xmm0, %%xmm0\n"
        "movq %%xmm0, %0\n"
        : "=r"(bits) : "r"(one) : "xmm0");
#endif

    return bits == 0x4000000000000000UL;    /* 2.0 as an IEEE-754 double */
}

#if defined(__aarch64__)
/*
 * Lazy FP save, from `arch/aarch64/fp.c`, and AArch64's alone.
 *
 * x86 saves the file *eagerly*, in `switch.S`, and `context.h` argues for
 * why: the lazy mechanism there would be CR0.TS and vector 7 and is
 * perfectly well understood, but the measurement that justified it on ARM -
 * 36% of a context switch - has not been taken on this board, and this
 * system does not push work down without one. So there is no owner to ask
 * about and nothing to find disarmed, and these two tests have nothing to
 * say there.
 *
 * What *is* checked on both is the guarantee rather than the mechanism:
 * `fp: a preemption preserves d0` holds a value across a spin long enough
 * to be preempted, and cannot tell which scheme carried it.
 */
void fp_reset(void);

/*
 * And that the laziness is real.
 *
 * A save that still happened on every switch would pass every other FP test
 * here: the registers would be correct, just expensively. What says
 * otherwise is the *state* - disarmed, with nobody owning the registers -
 * and that exactly one thing arms them.
 *
 * Driven directly rather than by yielding, because a yield with nothing
 * else runnable does not switch at all, and a test whose setup silently
 * does nothing is a test that passes for the wrong reason. That is what the
 * first version of this did.
 */
static bool test_fp_is_disarmed_until_it_is_wanted(void)
{
    uint64_t cpacr;
    uint64_t bits;

    /* The state a thread is handed by a context switch. */
    fp_reset();

    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));

    if (((cpacr >> 20) & 3) != 0) {
        return false;                       /* armed when it should not be */
    }

    if (fp_owned_by(thread_current())) {
        return false;                       /* owned when nobody asked */
    }

    /* One FP instruction, which traps, and comes back having worked. */
    __asm__ volatile(
        "fmov d0, #1.0\n"
        "fmov %0, d0\n"
        : "=r"(bits) : : "d0");

    if (bits != 0x3ff0000000000000UL) {     /* 1.0 */
        return false;
    }

    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));

    /* Armed now, and attributed to whoever wanted it. */
    return ((cpacr >> 20) & 3) == 3 && fp_owned_by(thread_current());
}
#endif /* __aarch64__ */

/*
 * setjmp and longjmp.
 *
 * The piece that has to be right the first time. Lua raises every error
 * through it, so a mistake is invisible until the first error is raised,
 * which can be weeks after this was written.
 */
static jmp_buf test_jmp;

static void jump_back(int value)
{
    longjmp(test_jmp, value);
}

static bool test_setjmp_returns_zero_directly(void)
{
    return setjmp(test_jmp) == 0;
}

static bool test_longjmp_delivers_its_value(void)
{
    volatile int seen = -1;
    int r = setjmp(test_jmp);

    if (r == 0) {
        jump_back(42);
    }

    seen = r;
    return seen == 42;
}

static bool test_longjmp_turns_zero_into_one(void)
{
    /* The standard requires it, and Lua depends on it: setjmp has to be able
     * to tell a direct call from an arrival. */
    int r = setjmp(test_jmp);

    if (r == 0) {
        jump_back(0);
    }

    return r == 1;
}

static volatile unsigned long seed = 0x1000;

static bool test_longjmp_restores_callee_saved_registers(void)
{
    /*
     * Ten values live across the jump, taken from a volatile so the compiler
     * cannot rematerialise them as constants. With that many it has to hold
     * some in x19 to x28, which is exactly the set longjmp must restore. If
     * it does not, they come back as whatever the deeper frame left there.
     *
     * This is the test that catches a jmp_buf whose offsets drifted from the
     * assembly.
     */
    unsigned long a = seed + 1, b = seed + 2, c = seed + 3, d = seed + 4;
    unsigned long e = seed + 5, f = seed + 6, g = seed + 7, h = seed + 8;
    unsigned long i = seed + 9, j = seed + 10;
    volatile bool jumped = false;

    if (setjmp(test_jmp) == 0) {
        jumped = true;
        jump_back(1);
    }

    return jumped
        && a == seed + 1 && b == seed + 2 && c == seed + 3 && d == seed + 4
        && e == seed + 5 && f == seed + 6 && g == seed + 7 && h == seed + 8
        && i == seed + 9 && j == seed + 10;
}

static void jump_from_a_deep_frame(void)
{
    /*
     * A big frame rather than a recursive one. Recursion would say the same
     * thing, but GCC refuses to compile it: the only non-recursive path ends
     * in longjmp, which is noreturn, so it concludes the function never
     * returns and reports infinite recursion. It is not wrong about what it
     * can see. Four kilobytes moves sp far enough to prove the point.
     */
    volatile char pad[4096];

    pad[0] = 1;
    pad[sizeof(pad) - 1] = 2;

    if (pad[0] + pad[sizeof(pad) - 1] == 3) {
        longjmp(test_jmp, 9);
    }
}

static bool test_longjmp_restores_the_stack_pointer(void)
{
    /*
     * Jumping out of a deep call has to put sp back where setjmp saw it. If
     * it does not, the stack keeps growing every time an error is raised,
     * and Lua raises a lot of them: the symptom is a stack overflow after
     * running fine for a while, which is the worst kind to chase.
     */
    volatile unsigned long before;
    volatile unsigned long after;
    int r;

    before = test_stack_pointer();

    r = setjmp(test_jmp);
    if (r == 0) {
        jump_from_a_deep_frame();
    }

    after = test_stack_pointer();

    return r == 9 && after == before;
}

/*
 * A translation changed on one processor, read on another.
 *
 * **The check x86 needed before a process's threads could be on two cores.**
 * A thread on core 1 reads a page through a user address, over and over, so
 * the translation sits in core 1's TLB. Core zero then changes what that
 * address maps, and core 1 has to read the change.
 *
 * On AArch64 `tlbi ...is` makes that true by itself. On x86 only the
 * shootdown in `arch/x86_64/mmu.c` does, and without it this fails: core 1
 * keeps reading the page core zero no longer maps there - not a fault, the
 * old contents, for as long as the entry stays cached.
 *
 * Both paths that change a live translation, and neither with the reader on
 * the address while it is unmapped - a read there would be a kernel fault:
 * the reader is moved to a second address first, the first is unmapped and
 * given another page, and the reader moved back; then the first is mapped
 * over while the reader is on it.
 */
static struct addrspace *volatile tlb_space;
static volatile uintptr_t         tlb_address;
static volatile uint32_t          tlb_seen;
static volatile unsigned long     tlb_reads;
static volatile bool              tlb_stop;
static volatile bool              tlb_done;

#define TLB_FIRST   0x0a11ce5eu
#define TLB_SECOND  0x0b0b0b0bu

static void tlb_reader(void *arg)
{
    unsigned long flags;

    (void)arg;

    thread_current()->space = tlb_space;
    as_switch(tlb_space);

    /*
     * **Interrupts masked for the whole read, and that is what makes this a
     * test.** With them on it passed with the shootdown switched off:
     * something on the interrupt path flushed core 1's TLB before a stale
     * entry could be seen. Masked, nothing flushes this core but a
     * shootdown, which it answers the way a core waiting for a lock does.
     */
    flags = cpu_interrupts_save();

    while (!tlb_stop) {
        uintptr_t at = tlb_address;

        /* Through a volatile pointer, so every pass is a load through the
         * TLB rather than one the compiler hoisted out of the loop. */
        tlb_seen = *(const volatile uint32_t *)at;
        tlb_reads++;

        cpu_lock_wait();
    }

    cpu_interrupts_restore(flags);

    as_switch(NULL);
    thread_current()->space = NULL;
    tlb_done = true;
}

/* Until the reader has read `want` for another `passes` passes. */
static bool tlb_reader_sees(uint32_t want, unsigned long passes)
{
    unsigned long mark = tlb_reads;
    unsigned long start = hal_ticks();

    while ((tlb_seen != want || tlb_reads < mark + passes)
           && hal_ticks() - start < 250UL) {
        cpu_relax();
    }

    return tlb_seen == want && tlb_reads >= mark + passes;
}

static bool test_a_changed_mapping_reaches_every_processor(void)
{
    const uintptr_t one = USER_VA_BASE;
    const uintptr_t two = USER_VA_BASE + PAGE_SIZE;
    uint32_t *first;
    uint32_t *second;
    unsigned long start;
    bool ok;

    if (smp_online() <= 1) {
        return true;            /* one processor, one TLB */
    }

    first = pmm_alloc_page();
    second = pmm_alloc_page();
    tlb_space = as_create();

    if (first == NULL || second == NULL || tlb_space == NULL) {
        return false;
    }

    *first = TLB_FIRST;
    *second = TLB_SECOND;

    if (as_map(tlb_space, one, (uintptr_t)first, 1, MAP_RW) != AS_OK
        || as_map(tlb_space, two, (uintptr_t)first, 1, MAP_RW) != AS_OK) {
        return false;
    }

    tlb_address = one;
    tlb_seen = 0;
    tlb_reads = 0;
    tlb_stop = false;
    tlb_done = false;

    if (thread_create_on(1, "tlb-reader", tlb_reader, NULL) == NULL) {
        return false;
    }

    /* Core 1 has the first address cached; then it moves to the second. */
    ok = tlb_reader_sees(TLB_FIRST, 10000UL);
    tlb_address = two;
    ok = ok && tlb_reader_sees(TLB_FIRST, 10000UL);

    /* Unmapped here and given the other page, while core 1 is elsewhere. */
    ok = ok && as_unmap(tlb_space, one, 1) == AS_OK
            && as_map(tlb_space, one, (uintptr_t)second, 1, MAP_RW) == AS_OK;

    tlb_address = one;
    ok = ok && tlb_reader_sees(TLB_SECOND, 10000UL);

    /* And mapped over while core 1 is reading it. */
    ok = ok && as_map(tlb_space, one, (uintptr_t)first, 1, MAP_RW) == AS_OK;
    ok = ok && tlb_reader_sees(TLB_FIRST, 10000UL);

    tlb_stop = true;
    start = hal_ticks();

    while (!tlb_done && hal_ticks() - start < 250UL) {
        cpu_relax();
    }

    /* A reader still running on these tables is a reason to leak them. */
    if (!tlb_done) {
        return false;
    }

    as_destroy(tlb_space);
    pmm_free_page(first);
    pmm_free_page(second);

    return ok;
}

/*
 * A reply to a caller on another core, two thousand times.
 *
 * **The bug this is here for stopped the whole desktop.** `ipc_call` woke
 * its receiver and only then joined the reply queue and named its endpoint.
 * A receiver on another core that answered at once read `waiting_on` with no
 * lock, found NULL, and dropped the reply as addressed to nobody - and the
 * caller blocked for ever. On one core the receiver cannot run before the
 * caller has blocked, which is why only spreading threads found it.
 *
 * So the server runs on core 1 and the caller on core 2, and every reply is
 * accounted for: the server counts the replies the kernel refused, and core
 * zero waits for the caller to finish its rounds. A lost reply fails this
 * rather than hanging the suite, because destroying the endpoint afterwards
 * wakes whatever is still waiting on it.
 */
#define REPLY_ROUNDS    2000u

static volatile cap_t    reply_server_cap;
static volatile cap_t    reply_caller_cap;
static volatile unsigned reply_refused;
static volatile unsigned reply_rounds_done;
static volatile bool     reply_caller_done;
static volatile bool     reply_server_done;

static void reply_server(void *arg)
{
    (void)arg;

    for (;;) {
        struct message msg;
        struct message reply;
        struct thread *sender;

        if (ipc_receive(reply_server_cap, &msg, &sender, false, 0) != IPC_OK) {
            break;              /* the endpoint was destroyed */
        }

        reply.tag = msg.tag + 1;
        reply.length = 0;

        if (ipc_reply(sender, &reply) != IPC_OK) {
            reply_refused++;
        }
    }

    reply_server_done = true;
}

static void reply_caller(void *arg)
{
    unsigned i;

    (void)arg;

    for (i = 0; i < REPLY_ROUNDS; i++) {
        struct message msg = { 0 };
        struct message reply = { 0 };

        msg.tag = i;
        msg.length = 0;

        if (ipc_call(reply_caller_cap, &msg, &reply) != IPC_OK
            || reply.tag != i + 1) {
            break;
        }

        reply_rounds_done++;
    }

    reply_caller_done = true;
}

static bool test_a_reply_reaches_a_caller_on_another_processor(void)
{
    struct thread *server;
    struct thread *caller;
    unsigned long start;
    cap_t ep;
    bool ok;

    if (smp_online() < 3) {
        return true;            /* a server and a caller need two other cores */
    }

    reply_refused = 0;
    reply_rounds_done = 0;
    reply_caller_done = false;
    reply_server_done = false;

    ep = ipc_endpoint_create();
    if (ep < 0) {
        return false;
    }

    server = thread_create_suspended("reply-server", reply_server, NULL);
    caller = thread_create_suspended("reply-caller", reply_caller, NULL);

    if (server == NULL || caller == NULL) {
        (void)ipc_endpoint_destroy(ep);
        return false;
    }

    reply_server_cap = ipc_cap_grant(server, ep);
    reply_caller_cap = ipc_cap_grant(caller, ep);

    if (reply_server_cap < 0 || reply_caller_cap < 0) {
        (void)ipc_endpoint_destroy(ep);
        return false;
    }

    /* Homes before the first wake, the way `thread_create_on` does it. */
    server->sched.cpu = 1;
    caller->sched.cpu = 2;
    thread_wake(server);
    thread_wake(caller);

    start = hal_ticks();

    while (!reply_caller_done && hal_ticks() - start < 2500UL) {
        cpu_relax();
    }

    ok = reply_caller_done && reply_rounds_done == REPLY_ROUNDS
      && reply_refused == 0;

    if (!ok) {
        kputs("\n   (");
        kputu(reply_rounds_done);
        kputs(" of ");
        kputu(REPLY_ROUNDS);
        kputs(" rounds, ");
        kputu(reply_refused);
        kputs(" replies refused)\n");
    }

    /* Ends the server's loop, and wakes a caller still waiting for a reply
     * that was lost. */
    (void)ipc_endpoint_destroy(ep);

    start = hal_ticks();

    while ((!reply_server_done || !reply_caller_done)
           && hal_ticks() - start < 250UL) {
        cpu_relax();
    }

    return ok;
}

/*
 * A thread exiting on one core while a thread is created on another.
 *
 * `thread_exit` marks a thread dead and then switches away from it, and until
 * that switch is over the thread is on its stacks with its registers unsaved.
 * `alloc_thread` hands out dead slots, stacks included, on any core. So a
 * thread created in that window was given the slot, had its context planted,
 * and then had the dying thread's registers saved over it: it started life
 * below the switch in `thread_exit`, and the kernel panicked with "a dead
 * thread was scheduled". One run of the x86 suite in eleven.
 *
 * This makes that instant likely:
 *
 * - Every slot is touched first. `alloc_thread` prefers a slot nothing has
 *   used, and while one exists the slot being left is never handed out. After
 *   that a new thread reuses the lowest dead slot, which is the one the
 *   exiting thread has just left.
 * - Core zero watches the exiting thread and creates a thread the moment it
 *   reads dead - on a third core, so the new thread can start while the old
 *   one is still switching away.
 * - The exiting thread leaves with an address space loaded, so its last
 *   switch changes page tables inside the window - more to do there than a
 *   switch between two kernel threads. A process that exits while its core's
 *   next thread belongs to another process makes the same switch in the same
 *   place.
 *
 * Failing is a panic rather than a false, because the thread that was given
 * the slot never gets back to say anything. Without the fix it did, in two
 * runs of two on each board.
 */
#define LEAVE_ROUNDS  200

static struct addrspace *volatile leave_space;
static volatile bool leave_armed;
static volatile bool leave_go;
static volatile bool reborn_ran;

static void leaves_with_a_space_loaded(void *arg)
{
    (void)arg;

    thread_current()->space = leave_space;
    as_switch(leave_space);

    leave_armed = true;

    while (!leave_go) {
        cpu_relax();
    }

    /* Returns with the space still loaded; see above. */
}

static void reborn(void *arg)
{
    (void)arg;
    reborn_ran = true;
}

/* Yielding rather than spinning, so a thread on this core gets to run. */
static bool wait_for_flag(const volatile bool *flag)
{
    unsigned long start = hal_ticks();

    while (!*flag && hal_ticks() - start < 250UL) {
        thread_yield();
    }

    return *flag;
}

/* The state read afresh on every pass rather than once, before the loop. */
static bool wait_until_dead(const struct thread *t, bool spin)
{
    unsigned long start = hal_ticks();

    while (*(const volatile enum thread_state *)&t->state != THREAD_DEAD
           && hal_ticks() - start < 250UL) {
        if (spin) {
            cpu_relax();
        } else {
            thread_yield();
        }
    }

    return t->state == THREAD_DEAD;
}

static bool test_a_slot_is_reused_only_once_its_thread_has_left(void)
{
    unsigned cores = smp_online();
    struct thread *touched[THREAD_MAX];
    unsigned before = thread_count();
    unsigned count = 0;
    unsigned round;
    unsigned long start;
    unsigned i;
    bool ok = true;

    if (cores < 2) {
        return true;        /* nothing runs between the mark and the switch */
    }

    leave_space = as_create();

    if (leave_space == NULL) {
        return false;
    }

    /* Every slot touched, and given straight back. */
    while (count < THREAD_MAX) {
        struct thread *t = thread_create_suspended("touch", short_thread, NULL);

        if (t == NULL) {
            break;
        }

        touched[count++] = t;
    }

    for (i = 0; i < count; i++) {
        thread_wake(touched[i]);
    }

    start = hal_ticks();

    while (thread_count() > before && hal_ticks() - start < 250UL) {
        thread_yield();
    }

    if (thread_count() > before) {
        return false;
    }

    for (round = 0; round < LEAVE_ROUNDS && ok; round++) {
        unsigned from = 1 + round % (cores - 1);
        unsigned to   = (cores > 2) ? 1 + from % (cores - 1) : 0;
        struct thread *leaver;
        struct thread *next;

        leave_armed = false;
        leave_go    = false;
        reborn_ran  = false;

        leaver = thread_create_on(from, "leaves", leaves_with_a_space_loaded, NULL);

        if (leaver == NULL || !wait_for_flag(&leave_armed)) {
            ok = false;
            break;
        }

        leave_go = true;

        /* The instant it reads dead, which is before it has left. */
        if (!wait_until_dead(leaver, true)) {
            ok = false;
            break;
        }

        next = thread_create_on(to, "reborn", reborn, NULL);

        ok = next != NULL
             && wait_for_flag(&reborn_ran)
             && wait_until_dead(next, false);
    }

    /* The last thread to leave was switching off this space; let it finish. */
    start = hal_ticks();

    while (hal_ticks() - start < 5UL) {
        thread_yield();
    }

    as_destroy(leave_space);
    leave_space = NULL;

    return ok;
}

static const struct test tests[] = {
    { "boot: .bss is zeroed",                  test_bss_zeroed          },
    { "boot: .bss bounds are 16-byte aligned", test_bss_bounds_aligned  },
    { "boot: running privileged",              test_running_at_the_privileged_level },
    { "boot: sp is inside the boot stack",     test_sp_inside_boot_stack },
    { "trap: an undefined instruction faults", test_undefined_instruction_faults },
    { "trap: brk reports EC 0x3c",             test_brk_faults },
#if defined(__aarch64__)
    /* The four that are about AArch64 rather than about the kernel. Every
     * other line in this table runs on both boards. */
    { "trap: elr is the faulting instruction", test_elr_points_at_the_faulting_instruction },
    { "trap: execution resumes after a fault", test_execution_resumes_after_an_expected_fault },
#endif
    { "trap: no fault means end() is false",   test_unexpected_fault_is_not_swallowed },
#if defined(__aarch64__)
    { "trap: the kernel runs on SP_EL0",       test_kernel_runs_on_sp_el0 },
#endif
    { "trap: the handler has its own stack",   test_handler_runs_on_the_exception_stack },
    { "trap: a stack overflow is survivable",  test_a_stack_overflow_is_survivable },
    { "thread: three threads interleave",      test_threads_interleave },
    { "thread: block and wake",                test_block_and_wake },
    { "thread: a switch preserves x19 and d8", test_context_switch_preserves_registers },
    { "thread: returning exits cleanly",       test_a_thread_that_returns_exits_cleanly },
    { "sched: the policy is pluggable",        test_the_scheduler_is_pluggable },
    { "thread: stacks have guard pages",       test_thread_stacks_have_guard_pages },
    { "el0: a process runs and exits",         test_a_process_runs_at_el0 },
    { "el0: Lua runs in a process",            test_lua_runs_at_el0 },
    { "el0: two processes swap a Lua table",   test_two_processes_exchange_a_lua_table },
    { "ns: same server, two names, two views", test_the_same_server_under_two_names },
    { "dev: only the owner may print",         test_only_the_console_owner_may_print },
    { "spawn: a child runs and is waited for", test_a_process_can_spawn_and_wait },
    { "el0: a null deref kills only it",       test_a_null_dereference_kills_only_the_process },
    { "el0: it cannot read the kernel",        test_a_process_cannot_read_the_kernel },
    { "el0: it cannot write its own code",     test_a_process_cannot_write_its_own_code },
    { "el0: a syscall refuses a kernel ptr",   test_a_syscall_refuses_a_kernel_pointer },
    { "el0: code shared, writable not",        test_processes_have_separate_address_spaces },
    { "as: a new space contains the kernel",   test_a_new_space_contains_the_kernel },
    { "as: map and unmap",                     test_a_space_maps_and_unmaps },
    { "as: the kernel region is refused",      test_a_space_refuses_the_kernel_region },
    { "lock: a spinlock excludes, names its holder and masks", test_a_spinlock_excludes_and_masks },
    { "cpu: the machine says how many processors it has", test_the_machine_says_how_many_processors_it_has },
    { "cpu: every processor claimed its own slot", test_every_processor_claimed_its_own_slot },
    { "cpu: every processor takes its own ticks",  test_every_processor_takes_its_own_ticks },
    { "cpu: every processor idles as a thread",     test_every_processor_idles_as_a_thread },
    { "smp: a thread runs on another processor",    test_a_thread_runs_on_another_processor },
    { "cpu: per-CPU state is per CPU, not per thread", test_percpu_is_per_cpu_and_not_per_thread },
    { "sched: a sleep lasts as long as it asked", test_a_sleep_lasts_as_long_as_it_asked },
    { "sched: input does not wake a plain sleeper", test_input_does_not_wake_a_plain_sleeper },
    { "sched: NORMAL runs while DISPLAY sleeps", test_normal_runs_while_a_higher_band_sleeps },
    { "as: a page count that wraps is refused", test_a_space_refuses_a_page_count_that_wraps },
    { "as: switching makes a mapping real",    test_switching_to_a_space_makes_its_mapping_real },
    { "as: destroy returns its pages",         test_destroying_a_space_returns_its_pages },
    { "sched: a spinning thread is preempted", test_a_thread_that_never_yields_is_preempted },
    { "sched: both sides keep running",        test_preemption_does_not_lose_the_preempted_thread },
    { "smp: a secondary preempts a spinner",   test_a_secondary_preempts_a_thread_that_never_yields },
    { "smp: a new thread avoids a loaded core", test_placement_avoids_a_loaded_processor },
    { "smp: a changed mapping reaches every core", test_a_changed_mapping_reaches_every_processor },
    { "smp: a reply reaches a caller on another core", test_a_reply_reaches_a_caller_on_another_processor },
    { "smp: a slot is reused only once its thread has left", test_a_slot_is_reused_only_once_its_thread_has_left },
    { "ipc: call and reply",                   test_ipc_call_and_reply },
    { "ipc: both arrival orders work",         test_ipc_works_in_both_arrival_orders },
    { "ipc: destroy wakes the blocked",        test_destroying_an_endpoint_wakes_the_blocked },
    { "ipc: a stale capability fails",         test_a_stale_capability_fails },
    { "ipc: an out-of-range index fails",      test_a_capability_index_out_of_range_fails },
    { "ipc: endpoints are reclaimed",          test_endpoints_are_reclaimed },
    { "ipc: a non-blocking receive returns",   test_a_nonblocking_receive_returns_empty },
    { "ipc: a receive with a deadline gives up", test_a_receive_with_a_deadline_gives_up },
    { "mmu: translation is on",                test_mmu_is_on },
    { "mmu: a null dereference faults",        test_null_dereference_faults },
    { "mmu: the stack guard is unmapped",      test_stack_guard_page_is_unmapped },
    { "mmu: kernel text is not writable",      test_kernel_text_is_not_writable },
    { "mmu: rodata is not writable",           test_rodata_is_not_writable },
    { "mmu: memory works through translation", test_memory_still_works_through_translation },
    { "libc: memcpy and memcmp",               test_memcpy_and_memcmp },
    { "libc: memset fills exactly its range",  test_memset_fills_exactly },
    { "libc: memmove handles overlap",         test_memmove_handles_overlap },
    { "libc: the string functions",            test_string_functions },
    { "fp: the kernel may use FP and SIMD",    test_fp_is_usable_in_the_kernel },
    { "sched: the higher priority runs first", test_higher_priority_runs_first },
    { "sched: a wake preempts a lower band",   test_a_wake_preempts_a_lower_priority_thread },
    { "sched: a server inherits its caller",   test_a_server_inherits_its_callers_priority },
    { "fp: a preemption preserves d0",         test_fp_survives_a_preemption },
#if defined(__aarch64__)
    { "fp: disarmed until it is wanted",       test_fp_is_disarmed_until_it_is_wanted },
#endif
    { "libc: setjmp returns 0 when called",    test_setjmp_returns_zero_directly },
    { "libc: longjmp delivers its value",      test_longjmp_delivers_its_value },
    { "libc: longjmp turns 0 into 1",          test_longjmp_turns_zero_into_one },
    { "libc: longjmp restores x19-x28",        test_longjmp_restores_callee_saved_registers },
    { "libc: longjmp restores sp",             test_longjmp_restores_the_stack_pointer },
    { "heap: allocations are 16-byte aligned", test_heap_alloc_is_aligned_and_usable },
    { "heap: free coalesces both ways",        test_heap_coalesces_in_both_directions },
    { "heap: realloc preserves contents",      test_heap_realloc_preserves_contents },
    { "heap: exhaustion returns NULL",         test_heap_exhaustion_returns_null },
    { "snprintf: integers and strings",        test_snprintf_integers_and_strings },
    { "snprintf: truncates, reports full len", test_snprintf_truncates_and_reports_the_full_length },
    { "snprintf: floats",                      test_snprintf_floats },
    { "snprintf: a float padded to a width",   test_snprintf_float_width },
    { "math: ours and newlib's agree",         test_our_math_matches_its_definition },
    { "lua: arithmetic, 2+2 is 4",             test_lua_arithmetic },
    { "lua: floats parse and print",           test_lua_floats },
    { "lua: strings and tables",               test_lua_strings_and_tables },
    { "lua: closures keep their upvalues",     test_lua_closures },
    { "lua: coroutines yield and resume",      test_lua_coroutines },
    { "lua: pcall catches errors",             test_lua_errors_are_caught },
    { "lua: nested pcall unwinds correctly",   test_lua_errors_nest },
    { "lua: the math library",                 test_lua_math_library },
    { "lua: the collector reclaims memory",    test_lua_gc_reclaims },
    { "lua: io, os, debug are absent",         test_lua_dangerous_libs_absent },
    { "lua: load refuses bytecode",            test_lua_refuses_bytecode },
    { "ipc: a process answers over IPC",       test_a_process_answers_over_ipc },
    { "ipc: destroy reaches a process",        test_destroying_an_endpoint_reaches_a_process },
    { "ipc: a Lua table survives IPC",         test_a_lua_table_survives_ipc },
    { "ipc: the serialiser refuses the rest",  test_the_serialiser_refuses_what_cannot_cross },
    { "cap: a capability travels in a message", test_a_capability_can_be_passed_in_a_message },
    { "cap: one you do not hold does not",      test_a_capability_that_is_not_held_cannot_be_sent },
    { "ipc: errors reach Lua",                 test_lua_ipc_errors_are_reported },
    { "blk: the disk is there",                test_block_device_is_present },
    { "blk: a sector reads back what was written", test_block_write_then_read },
    { "blk: sectors are addressed, not ignored", test_block_sectors_are_distinct },
    { "blk: past the end is refused",          test_block_refuses_past_the_end },
    { "gfx: triangles fill and meet",          test_gfx_triangles },
    { "3d: the near faces are the drawn ones", test_g3d_orientation },
    { "kill: a sibling may not be ended",      test_kill_is_parent_only },
    { "cap: forty regions, made and released", test_cap_release_frees_slots },
    { "inflate: a stream from elsewhere",      test_inflate_round_trip },
    { "pdf: the scanner reads what it should", test_pdf_scanner },
    { "con: a write carries no capability",    test_console_write_carries_no_capability },
    { "mem: a shared region is freed once",     test_shared_memory_is_freed_once },
    { "as: one space per possible process",    test_enough_address_spaces_for_every_process },
    { "input: the keyboard came up",           test_the_keyboard_came_up },
    { "boot: every stage was announced",       test_the_boot_announced_every_stage },
    { "fb: the display comes up",              test_the_display_comes_up },
    { "console: a write carries its colour, and UTF-8",
                                               test_console_colour_and_utf8 },
    { "fb: the pitch is not width * 4",        test_the_pitch_is_not_the_width },
    { "fb: page aligned and inside RAM",       test_the_framebuffer_is_page_aligned_and_in_ram },
    { "fb: every row is writable",             test_the_framebuffer_is_writable_end_to_end },
    { "fb: the padding actually moves rows",   test_the_padding_is_real },
    { "fb: pmm never hands out its pages",     test_the_page_allocator_never_hands_out_the_framebuffer },
    { "gfx: surfaces clip, free and honour pitch", test_gfx_surfaces },
    { "gfx: the alpha multiply is exact",      test_gfx_alpha_is_exact },
    { "gfx: no screen unless it was granted",  test_a_process_without_the_screen },
    { "gfx: the font rasterises correctly",    test_gfx_text },
    { "irq: interrupts are unmasked",          test_irqs_are_unmasked },
    { "timer: ticks advance",                  test_timer_ticks_advance },
    { "timer: the period matches the rate",    test_timer_period_matches_the_rate },
    { "pmm: a page is aligned and in RAM",     test_pmm_alloc_is_page_aligned_and_in_ram },
    { "pmm: two allocations differ",           test_pmm_two_allocations_differ },
    { "pmm: the count tracks alloc and free",  test_pmm_count_tracks_alloc_and_free },
    { "pmm: a page is writable end to end",    test_pmm_page_is_writable },
    { "pmm: kernel pages are never free",      test_pmm_kernel_pages_are_not_free },
    { "pmm: exhaustion returns NULL",          test_pmm_exhaustion_returns_null },
};

#define TEST_COUNT (sizeof(tests) / sizeof(tests[0]))

/*
 * A heap, for the tests that are about the heap.
 *
 * The kernel has none. It allocated 2 MB once, for a `lua_State` it opened
 * itself, and both went when Lua did; nothing in kernel/, arch/ or hal/
 * calls malloc. `runtime/libc/malloc.c` is still ours and still needs unit
 * tests, so the suite brings up a heap of its own and the shipping image
 * pays nothing for it.
 *
 * Small on purpose. `test_heap_exhaustion_returns_null` asks for one byte
 * more than the whole thing, and a 2 MB request would be a slower way of
 * learning the same fact.
 */
#define TEST_HEAP_PAGES 64      /* 256 KB at a 4 KB granule */

void tests_run(void)
{
    unsigned failed = 0;
    void *heap;

    /*
     * Everything this suite creates comes home to this processor.
     *
     * **Not a hedge, and not a limitation of the kernel** - a machine
     * somebody uses spreads threads across every core that came up. It is
     * the honest reading of what most of these checks ask. A dozen of them
     * mask interrupts, create three threads and drive them by yielding, and
     * that is a question about *this* processor's scheduler which only means
     * anything if those threads are here. Spreading them would not make the
     * tests better; it would make them stop asking.
     *
     * The checks that are about SMP say so by using `thread_create_on`, and
     * they still cross cores. `smp: a thread runs on another processor` is
     * the one that matters, and this line is what makes it a deliberate
     * crossing rather than an accident of placement.
     */
    thread_place_across(1);

    heap = pmm_alloc_contiguous(TEST_HEAP_PAGES);

    if (heap == NULL) {
        kputs("not ok 1 - the suite could not get a heap\n");
        tests_exit(1);
    }

    heap_init(heap, TEST_HEAP_PAGES * PAGE_SIZE);


    /* TAP wants the plan before the results, so a truncated run is
     * detectable: the host counts what arrived against what was promised. */
    kputs("1..");
    kputu(TEST_COUNT);
    kputs("\n");

    for (unsigned i = 0; i < TEST_COUNT; i++) {
        bool ok = tests[i].fn();

        if (!ok) {
            failed++;
        }

        kputs(ok ? "ok " : "not ok ");
        kputu(i + 1);
        kputs(" - ");
        kputs(tests[i].name);
        kputs("\n");
    }

    tests_exit(failed == 0 ? 0 : 1);
}
