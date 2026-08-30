#include <stdbool.h>
#include <stdint.h>

#include "test.h"
#include "console.h"
#include "semihosting.h"
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

#include <string.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

/* The Kosmos configuration first, exactly as the Lua build does it. Every
 * hook it overrides is guarded upstream by #if !defined, so arriving after
 * lua.h means upstream's default wins and this one is a redefinition. */
#include "kosmos_lua.h"
#include "lua.h"
#include "lauxlib.h"

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

static bool test_running_at_el1(void)
{
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return ((el >> 2) & 3) == 1;
}

static bool test_sp_inside_boot_stack(void)
{
    /*
     * The stack lives above __bss_end, so this failing means either that the
     * linker script let the two overlap or that start.S never set sp.
     */
    uint64_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));

    return sp > (uint64_t)(uintptr_t)__stack_bottom
        && sp <= (uint64_t)(uintptr_t)__stack_top;
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

    fault_expect_begin();
    __asm__ volatile("udf #0");

    if (!fault_expect_end(&f)) {
        return false;
    }

    /* An undefined instruction reports EC 0, "unknown reason". */
    return ESR_EC(f.esr) == EC_UNKNOWN;
}

static bool test_brk_faults(void)
{
    struct fault_info f;

    fault_expect_begin();
    __asm__ volatile("brk #0");

    return fault_expect_end(&f) && ESR_EC(f.esr) == EC_BRK64;
}

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

/*
 * A store the compiler is not allowed to reason about.
 *
 * Writing `*(volatile int *)0 = 1` in C does not work. Dereferencing a null
 * pointer is undefined behaviour, so GCC is entitled to assume it cannot
 * happen: it emits the store and then treats everything after it as
 * unreachable, appending a `brk`. The store faults, the handler steps elr
 * past it, and execution lands on the brk. That is a second fault with the
 * expectation already spent, and the kernel panics.
 *
 * Found exactly that way. In assembly the store is just a store.
 */
static void store_to(uintptr_t addr, uint32_t value)
{
    __asm__ volatile("str %w0, [%1]" : : "r"(value), "r"(addr) : "memory");
}

/* Any translation fault, whatever level it happened at. Which level depends
 * on where the walk ran out of table, which is a detail of how the map was
 * built rather than a property worth asserting on. */
static bool is_translation_fault(unsigned iss)
{
    return (ISS_DABT_DFSC(iss) >> 2) == 1;
}

static bool is_permission_fault(unsigned iss)
{
    return (ISS_DABT_DFSC(iss) >> 2) == 3;
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
    unsigned iss;

    fault_expect_begin();
    store_to(0, 1);

    if (!fault_expect_end(&f)) {
        return false;
    }

    iss = (unsigned)ESR_ISS(f.esr);

    return ESR_EC(f.esr) == EC_DABT_SAME
        && ISS_DABT_WNR(iss) == 1       /* it was a write */
        && is_translation_fault(iss)
        && f.far == 0;                  /* and it names the address */
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

    fault_expect_begin();
    store_to((uintptr_t)__stack_guard, 1);

    return fault_expect_end(&f)
        && is_translation_fault((unsigned)ESR_ISS(f.esr))
        && f.far == (uint64_t)(uintptr_t)__stack_guard;
}

static bool test_kernel_text_is_not_writable(void)
{
    /* W^X in the kernel: .text is mapped read-only and executable. A write
     * has to be a permission fault, not a translation fault, because the
     * page is mapped. */
    extern char __text_start[];
    struct fault_info f;

    fault_expect_begin();
    store_to((uintptr_t)__text_start, 0);

    return fault_expect_end(&f)
        && is_permission_fault((unsigned)ESR_ISS(f.esr))
        && f.far == (uint64_t)(uintptr_t)__text_start;
}

static bool test_rodata_is_not_writable(void)
{
    extern char __rodata_start[];
    struct fault_info f;

    fault_expect_begin();
    store_to((uintptr_t)__rodata_start, 0);

    return fault_expect_end(&f)
        && is_permission_fault((unsigned)ESR_ISS(f.esr));
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
     * false when nothing fired, so a broken test cannot pass by accident. */
    fault_expect_begin();
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
     */
    unsigned i;

    trace_len = 0;

    for (i = 1; i <= 3; i++) {
        if (thread_create("count", counting_thread, (void *)(uintptr_t)i) == NULL) {
            return false;
        }
    }

    /* Let them run. Each needs three turns, and this thread yields between
     * each round, so a handful of yields is more than enough. */
    for (i = 0; i < 16; i++) {
        thread_yield();
    }

    if (trace_len != 9) {
        return false;
    }

    /* Every id appears exactly three times, and no id appears twice in a row,
     * which is what "interleaved" actually means. */
    {
        unsigned counts[4] = { 0, 0, 0, 0 };

        for (i = 0; i < 9; i++) {
            if (trace[i] < 1 || trace[i] > 3) {
                return false;
            }
            counts[trace[i]]++;
            if (i > 0 && trace[i] == trace[i - 1]) {
                return false;
            }
        }

        return counts[1] == 3 && counts[2] == 3 && counts[3] == 3;
    }
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
     * Plants known values in a callee-saved general register and a
     * callee-saved FP register, yields so a switch definitely happens, and
     * reads them back.
     *
     * The FP half is the one that matters. The kernel's own C is built
     * -mgeneral-regs-only and never touches d8, but Lua runs on a thread and
     * its numbers are doubles. A switch that forgets d8 corrupts a value
     * with no trace of where it happened, which is exactly the bug that
     * surfaces five functions later.
     */
    uint64_t x;
    double d;

    __asm__ volatile("mov x19, #0x1234" ::: "x19");
    __asm__ volatile("fmov d8, #2.0" ::: "d8");

    thread_yield();
    thread_yield();

    __asm__ volatile("mov %0, x19" : "=r"(x));
    __asm__ volatile("fmov %d0, d8" : "=w"(d));

    saved_x19 = x;
    saved_d8 = d;
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
static struct thread *lifo_top;

static void lifo_init(void)   { lifo_top = NULL; }

static void lifo_enqueue(struct thread *t)
{
    t->sched.next = lifo_top;
    lifo_top = t;
}

static struct thread *lifo_pick_next(void)
{
    struct thread *t = lifo_top;

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
    unsigned i;

    /* Swapping policies is only safe with nothing queued, which is why this
     * runs each set of threads to completion before the next. */
    sched_use(policy);
    trace_len = 0;

    for (i = 1; i <= 3; i++) {
        if (thread_create("order", order_thread, (void *)(uintptr_t)i) == NULL) {
            return false;
        }
    }

    for (i = 0; i < 12; i++) {
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

    /* Put the real policy back before anything else runs. */
    sched_use(&sched_round_robin);

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
    struct process *p = process_create(name, start, (size_t)(end - start));
    unsigned i;
    int code;

    if (p == NULL) {
        return -1000;
    }

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
     * It exercises arithmetic and floats, math.sqrt out of newlib's libm,
     * coroutines, an error raised inside the VM and caught by pcall, the
     * absence of io, os and debug, and the collector reclaiming memory on a
     * heap it cannot grow. Any of those failing makes the chunk raise, which
     * makes main return non-zero.
     *
     * Its output goes to the console rather than into an assertion, because
     * what is being asserted is that all of it ran at EL0 and came back with
     * a zero.
     */
    extern const unsigned char init_image[];
    extern const unsigned long init_image_len;
    unsigned before = process_count();

    struct process *p = process_create("t-init", init_image,
                                       (size_t)init_image_len);
    unsigned i;
    int code;

    if (p == NULL) {
        return false;
    }

    for (i = 0; i < 4096 && !p->exited; i++) {
        thread_yield();
    }

    if (!p->exited) {
        return false;
    }

    code = p->exit_code;
    process_reap(p);

    return code == 0 && process_count() == before;
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
     * Two processes, both mapping their code at USER_TEXT_VA, both reaching
     * a different physical page. Checked from the kernel by walking each
     * space rather than from inside them, since neither can see the other
     * by construction, which is the point.
     */
    struct process *a;
    struct process *b;
    uint64_t *ea;
    uint64_t *eb;
    bool ok;
    unsigned i;

    a = process_create("t-sp-a", user_hello_start,
                       (size_t)(user_hello_end - user_hello_start));
    b = process_create("t-sp-b", user_hello_start,
                       (size_t)(user_hello_end - user_hello_start));

    if (a == NULL || b == NULL) {
        return false;
    }

    ea = as_page_entry(a->space, USER_TEXT_VA);
    eb = as_page_entry(b->space, USER_TEXT_VA);

    ok = ea != NULL && eb != NULL
      && (*ea & 1) != 0 && (*eb & 1) != 0
      && (*ea & DESC_ADDR_MASK) != (*eb & DESC_ADDR_MASK);

    /* Their stacks are different pages too, so nothing is shared by
     * accident rather than only the thing that was checked first. */
    {
        uintptr_t stack_page = USER_STACK_TOP - PAGE_SIZE;
        uint64_t *sa = as_page_entry(a->space, stack_page);
        uint64_t *sb = as_page_entry(b->space, stack_page);

        ok = ok && sa != NULL && sb != NULL
                && (*sa & 1) != 0 && (*sb & 1) != 0
                && (*sa & DESC_ADDR_MASK) != (*sb & DESC_ADDR_MASK);
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
            && (*entry & 1) != 0
            && (*entry & DESC_ADDR_MASK) == (uintptr_t)page;

    /* And the kernel's own map is untouched: this is a different space. */
    ok = ok && mmu_page_entry(USER_VA_BASE) == NULL;

    ok = ok && as_unmap(as, USER_VA_BASE, 1) == AS_OK;
    entry = as_page_entry(as, USER_VA_BASE);
    ok = ok && entry != NULL && (*entry & 1) == 0;

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

    ok = as_map(as, 0x40000000UL, 0x40000000UL, 1, MAP_RW) == AS_ERR_RANGE
      && as_map(as, 0x09000000UL, 0x09000000UL, 1, MAP_RW) == AS_ERR_RANGE
      && as_map(as, USER_VA_BASE + 1, 0x40000000UL, 1, MAP_RW) == AS_ERR_ALIGN
      && as_map(as, USER_VA_END, 0x40000000UL, 1, MAP_RW) == AS_ERR_RANGE;

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
    fault_expect_begin();
    store_to(USER_VA_BASE, 1);
    ok = ok && fault_expect_end(&f)
            && is_translation_fault((unsigned)ESR_ISS(f.esr))
            && f.far == USER_VA_BASE;

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
        return false;   /* it allocated nothing, so there is nothing to test */
    }

    as_destroy(as);

    return pmm_free_pages() == before;
}

/*
 * M3: preemption.
 */

/* Defined with the timer tests further down; used here to bound a wait by
 * elapsed time rather than by a loop count, so a kernel without preemption
 * fails quickly instead of hanging. */
static uint64_t cntfrq(void);
static uint64_t cntpct(void);

static volatile bool          spinner_stop;
static volatile unsigned long spinner_laps;

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
        unsigned w;

        server_result = ipc_receive(server_cap, &msg, &sender);
        if (server_result != IPC_OK) {
            break;
        }

        reply.tag = msg.tag + 1;
        for (w = 0; w < MSG_WORDS; w++) {
            reply.word[w] = msg.word[w] * 2;
        }

        server_result = ipc_reply(sender, &reply);
        if (server_result != IPC_OK) {
            break;
        }
    }

    server_done = true;
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

    server = thread_create("echo", echo_server, (void *)(uintptr_t)1);
    if (server == NULL) {
        return false;
    }

    /* The server gets its own index for the same endpoint. The two are
     * unrelated on purpose: an index means nothing outside its own table. */
    server_cap = ipc_cap_grant(server, client_cap);
    if (server_cap < 0) {
        return false;
    }

    msg.tag = 7;
    for (i = 0; i < MSG_WORDS; i++) {
        msg.word[i] = i + 1;
    }

    if (ipc_call(client_cap, &msg, &reply) != IPC_OK) {
        return false;
    }

    if (reply.tag != 8) {
        return false;
    }

    for (i = 0; i < MSG_WORDS; i++) {
        if (reply.word[i] != (uint64_t)(i + 1) * 2) {
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

    server = thread_create("echo2", echo_server, (void *)(uintptr_t)2);
    if (server == NULL) {
        return false;
    }
    server_cap = ipc_cap_grant(server, cap);

    /* Sender first: the server has not run at all yet. */
    msg.tag = 1;
    msg.word[0] = 10;
    ok = (ipc_call(cap, &msg, &reply) == IPC_OK) && reply.word[0] == 20;

    /* Receiver first: give the server a chance to be waiting. */
    for (i = 0; i < 4; i++) {
        thread_yield();
    }

    msg.tag = 2;
    msg.word[0] = 30;
    ok = ok && (ipc_call(cap, &msg, &reply) == IPC_OK) && reply.word[0] == 60;

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

    caller = thread_create("caller", blocked_caller, NULL);
    if (caller == NULL) {
        return false;
    }

    caller_cap = ipc_cap_grant(caller, cap);
    if (caller_cap < 0) {
        return false;
    }

    /* The thread was created before its capability index was known, so hand
     * it over through the argument slot the entry function reads. */
    caller->ctx.x20 = (uint64_t)(uintptr_t)(intptr_t)caller_cap;

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

static bool test_kernel_runs_on_sp_el0(void)
{
    /* SPSel bit 0: 0 means SP_EL0 is the current stack pointer. The whole
     * separation rests on this being 0 in ordinary code. */
    uint64_t spsel;
    __asm__ volatile("mrs %0, spsel" : "=r"(spsel));
    return (spsel & 1) == 0;
}

static bool test_handler_runs_on_the_exception_stack(void)
{
    /*
     * Direct evidence that the two stacks are actually separate, rather than
     * the code merely looking as though it set them up that way.
     */
    struct fault_info f;

    fault_expect_begin();
    __asm__ volatile("udf #0");

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
    __asm__ volatile("mov %0, sp" : "=r"(sp_after));

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

    /* And newlib's half, so a broken link shows up here rather than inside
     * Lua's arithmetic. */
    return pow(2.0, 10.0) == 1024.0 && fmod(7.0, 3.0) == 1.0;
}

/*
 * M2: Lua.
 *
 * One state, opened on first use and kept. Opening one per test would be a
 * better isolation story and a worse test: the interesting failures are the
 * ones that only appear after the collector has run a few times over a heap
 * that has been used.
 */
static struct lua_State *shared;

static struct lua_State *lua_state(void)
{
    if (shared == NULL) {
        shared = kosmos_lua_open();
    }

    return shared;
}

/* Runs a chunk that returns a boolean, and reports what it returned. A chunk
 * that fails to compile or raises is a false, which is what a test wants. */
static bool lua_says_true(const char *src)
{
    struct lua_State *L = lua_state();
    bool ok;

    if (L == NULL) {
        return false;
    }

    if (kosmos_lua_dostring(L, "=test", src) != LUA_OK) {
        lua_pop((lua_State *)L, 1);
        return false;
    }

    ok = lua_toboolean((lua_State *)L, -1) != 0;
    lua_pop((lua_State *)L, 1);
    return ok;
}

static bool test_lua_arithmetic(void)
{
    /* M2's definition of done, as an assertion rather than a printed line. */
    return lua_says_true("return 2 + 2 == 4")
        && lua_says_true("return 7 // 2 == 3 and 7 % 2 == 1")
        && lua_says_true("return math.type(1) == 'integer'")
        && lua_says_true("return math.type(1.0) == 'float'");
}

static bool test_lua_floats(void)
{
    /*
     * Exercises the whole number path at once: the parser calls strtod on
     * the literal, the arithmetic runs in FP registers that only exist
     * because CPACR was opened, and tostring goes back out through our
     * snprintf.
     */
    return lua_says_true("return 1 / 2 == 0.5")
        && lua_says_true("return tostring(1.5) == '1.5'")
        && lua_says_true("return tostring(0.25) == '0.25'")
        && lua_says_true("return tonumber('3.5') == 3.5")
        && lua_says_true("return tonumber('1e3') == 1000.0")
        && lua_says_true("return 2.0 ^ 10 == 1024.0");
}

static bool test_lua_strings_and_tables(void)
{
    return lua_says_true("return ('a' .. 'b' .. 'c') == 'abc'")
        && lua_says_true("return #'hello' == 5")
        && lua_says_true("return ('hello'):upper() == 'HELLO'")
        && lua_says_true("return string.format('%d-%s', 7, 'x') == '7-x'")
        && lua_says_true("local t = {} for i=1,100 do t[i]=i*i end return t[10] == 100 and #t == 100")
        && lua_says_true("local s=0 for _,v in ipairs{1,2,3} do s=s+v end return s == 6");
}

static bool test_lua_closures(void)
{
    return lua_says_true(
        "local function counter() local n = 0 "
        "  return function() n = n + 1 return n end end "
        "local c = counter() c() c() return c() == 3");
}

static bool test_lua_coroutines(void)
{
    /*
     * The single most important Lua feature for this design. `design.md`
     * §4.5 rests the entire server model on coroutines: they are what makes
     * synchronous IPC writable as sequential code instead of a state
     * machine. If they did not work, the architecture would not.
     */
    return lua_says_true(
        "local co = coroutine.create(function(a) "
        "  local b = coroutine.yield(a + 1) "
        "  return b * 2 end) "
        "local _, x = coroutine.resume(co, 1) "
        "local _, y = coroutine.resume(co, 10) "
        "return x == 2 and y == 20 and coroutine.status(co) == 'dead'");
}

static bool test_lua_errors_are_caught(void)
{
    /*
     * This is the setjmp/longjmp test that matters. The unit tests for them
     * jump within one function; here Lua raises from inside its own VM,
     * across frames it built, and unwinds to a pcall. `setup.md` warns that
     * a wrong longjmp only shows up the first time something raises, and
     * this is that first time.
     */
    return lua_says_true("local ok, e = pcall(function() error('boom') end) "
                         "return ok == false and e:find('boom') ~= nil")
        && lua_says_true("local ok = pcall(function() return nil + 1 end) "
                         "return ok == false")
        && lua_says_true("local ok, e = pcall(function() error({code=42}) end) "
                         "return ok == false and e.code == 42")
        /* And that the state is still usable afterwards, which is the part
         * a botched stack restore breaks. */
        && lua_says_true("return 1 + 1 == 2");
}

static bool test_lua_errors_nest(void)
{
    /* A pcall inside a pcall, with the inner one rethrowing. Nested jmp_bufs
     * are where an incorrect saved sp shows up as a corrupted outer frame. */
    return lua_says_true(
        "local ok, e = pcall(function() "
        "  local ok2 = pcall(function() error('inner') end) "
        "  error('outer:' .. tostring(ok2)) end) "
        "return ok == false and e:find('outer:false') ~= nil");
}

static bool test_lua_math_library(void)
{
    /* Ours and newlib's, reached through Lua rather than called directly. */
    return lua_says_true("return math.floor(2.7) == 2 and math.ceil(2.1) == 3")
        && lua_says_true("return math.abs(-3) == 3")
        && lua_says_true("return math.sqrt(16.0) == 4.0")
        && lua_says_true("return math.max(1,5,3) == 5")
        && lua_says_true("return math.fmod(7, 3) == 1.0")
        && lua_says_true("return type(math.random()) == 'number'");
}

static bool test_lua_garbage_collector_reclaims(void)
{
    /*
     * The GC is the design's known risk (`design.md` §5.2) and the first
     * thing to check is simply that it gets memory back at all. A collector
     * running against an allocator that does not coalesce would show up
     * here as memory that never drops.
     *
     * Four thousand tables, not twenty. Twenty thousand runs the 2 MB heap
     * out, which is itself worth knowing: a Lua table is around 56 bytes and
     * this allocator adds 32 more per block, so the overhead is not a
     * rounding error. That is the number roadmap.md M4 puts on the bench.
     */
    return lua_says_true(
        "collectgarbage() "
        "local before = collectgarbage('count') "
        "local t = {} for i = 1, 4000 do t[i] = {i} end "
        "local peak = collectgarbage('count') "
        "t = nil "
        "collectgarbage() "
        "local after = collectgarbage('count') "
        "return peak > before * 2 and after < peak / 2");
}

/*
 * M3: the kernel, reached from Lua.
 */
static bool test_sys_reports_the_kernel(void)
{
    return lua_says_true("return type(sys) == 'table'")
        && lua_says_true("return sys.scheduler() == 'round-robin'")
        && lua_says_true("local m = sys.memory() "
                         "return m.pages_free > 0 and m.pages_free < m.pages_total "
                         "and m.heap_used > 0 and m.heap_used < m.heap_size")
        && lua_says_true("local t = sys.threads() "
                         "if #t < 1 then return false end "
                         "local running = 0 "
                         "for _, th in ipairs(t) do "
                         "  if th.current then running = running + 1 end "
                         "  if type(th.name) ~= 'string' then return false end "
                         "end "
                         "return running == 1");
}

static bool test_a_spawned_lua_thread_answers_over_ipc(void)
{
    /*
     * The whole of M3 in one assertion: two Lua states, each in its own
     * kernel thread, exchanging messages through the microkernel's IPC.
     *
     * Separate states rather than one shared is not a convenience. A
     * lua_State is not reentrant, so two kernel threads inside one would
     * corrupt it. `design.md` §2's share-nothing userland is not a
     * preference here, it is the only thing that works.
     */
    return lua_says_true(
        "local ep = sys.endpoint() "
        "local ok = sys.spawn('t-double', [[ "
        "  local cap = ... "
        "  while true do "
        "    local m, who = sys.receive(cap) "
        "    if not m then break end "
        "    sys.reply(who, { m[1] * 2, tag = m.tag + 1 }) "
        "  end ]], ep) "
        "if not ok then sys.destroy(ep) return false end "
        "local good = true "
        "for i = 1, 5 do "
        "  local r = sys.call(ep, { i, tag = i }) "
        "  if not r or r[1] ~= i * 2 or r.tag ~= i + 1 then good = false end "
        "end "
        "sys.destroy(ep) "
        "for _ = 1, 8 do sys.yield() end "
        "return good");
}

static bool test_destroying_an_endpoint_reaches_a_lua_server(void)
{
    /*
     * The milestone's trap, seen from the language rather than from C. A
     * server blocked in receive has to come back with an error when its
     * endpoint goes away, or it waits forever and can never be restarted.
     *
     * The server records what it saw in a capability it was also handed, so
     * the answer comes back rather than being inferred from a thread state.
     */
    return lua_says_true(
        "local ep = sys.endpoint() "
        "if not sys.spawn('t-block', [[ "
        "  local cap = ... "
        "  local m, who = sys.receive(cap) "
        "  if m ~= nil then error('receive should have failed') end ]], ep) "
        "then sys.destroy(ep) return false end "
        /* Let it reach the receive and block. */
        "for _ = 1, 4 do sys.yield() end "
        "local blocked = false "
        "for _, t in ipairs(sys.threads()) do "
        "  if t.name == 't-block' and t.state == 'blocked' then blocked = true end "
        "end "
        "if not blocked then sys.destroy(ep) return false end "
        "sys.destroy(ep) "
        "for _ = 1, 8 do sys.yield() end "
        /* It woke, its receive failed, and it exited rather than hanging. */
        "for _, t in ipairs(sys.threads()) do "
        "  if t.name == 't-block' then return false end "
        "end "
        "return true");
}

static bool test_lua_ipc_errors_are_reported(void)
{
    return lua_says_true("local r, e = sys.call(99, {1}) "
                         "return r == nil and e == 'no such capability'")
        && lua_says_true("local r, e = sys.destroy(99) "
                         "return r == nil and e ~= nil");
}

static bool test_lua_dangerous_libraries_are_absent(void)
{
    /*
     * `design.md` §5.3: the list of libraries inside a state is part of the
     * security model, not a configuration detail. Asserting the absences
     * keeps a later "just open everything" from passing quietly.
     */
    return lua_says_true("return io == nil")        /* no global tree to open */
        && lua_says_true("return os == nil")        /* no wall clock, no exec */
        && lua_says_true("return debug == nil")     /* breaks every abstraction */
        && lua_says_true("return package == nil")   /* wants dlopen */
        && lua_says_true("return dofile == nil and loadfile == nil");
}

static bool test_lua_refuses_bytecode(void)
{
    /*
     * `design.md` §5.3 forbids precompiled bytecode outright: the undump
     * loader validates almost nothing, so a crafted chunk is arbitrary
     * execution inside the state.
     *
     * Upstream's default mode is "bt", which accepts either, so this is only
     * true because every load in lua/kosmos/ says "t". The test is here to
     * fail the day someone drops the argument.
     */
    return lua_says_true(
        "local dumped = string.dump(function() return 1 end) "
        "local f, err = load(dumped, 'evil', 't') "
        "return f == nil and err ~= nil")
        /* and that the same chunk as source loads fine, so the test is
         * really about the mode and not about load being broken */
        && lua_says_true("return load('return 1', 'ok', 't')() == 1");
}

/*
 * M1: the interrupt controller and the timer.
 */
static uint64_t cntfrq(void)
{
    uint64_t hz;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));
    return hz;
}

static uint64_t cntpct(void)
{
    uint64_t now;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
    return now;
}

static bool test_irqs_are_unmasked(void)
{
    /* DAIF bit 7 is the I mask. start.S enters EL1 with everything masked;
     * kmain clears it once there is a handler and a source. */
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    return (daif & (1UL << 7)) == 0;
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
    unsigned long k0;
    unsigned long k1;
    uint64_t t0;
    uint64_t elapsed;
    uint64_t expected;
    uint64_t tolerance;

    /* Start on a tick boundary so the first period is a whole one. */
    k0 = hal_ticks();
    while (hal_ticks() == k0) { }

    k0 = hal_ticks();
    t0 = cntpct();

    while (hal_ticks() - k0 < 100) { }

    elapsed = cntpct() - t0;
    k1 = hal_ticks();

    if (k1 <= k0) {
        return false;
    }

    /*
     * Divided by the ticks that actually elapsed rather than by the number
     * this loop was waiting for. Since preemption arrived, the measuring
     * thread can be taken off the CPU inside that loop and notice the last
     * tick up to a whole quantum late, which on a short window is a large
     * error in a measurement that is supposed to be about the timer.
     * Sampling both ends together removes the observation error rather than
     * budgeting for it.
     */
    expected  = (cntfrq() / TICK_HZ) * (k1 - k0);
    tolerance = expected / 10;

    return elapsed > expected - tolerance
        && elapsed < expected + tolerance;
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

static bool test_fp_is_usable_at_el1(void)
{
    /*
     * CPACR_EL1.FPEN resets to "trap everything", and setjmp saves d8 to
     * d15. Without the enable in start.S, the first setjmp is an EC 0x07
     * trap whose name says nothing about floating point unless you go
     * looking. That is how it was found.
     *
     * Reading CPACR is not enough on its own, so this also executes an FP
     * instruction. The arithmetic is in assembly because the kernel is built
     * -mgeneral-regs-only and the compiler will not emit one.
     */
    uint64_t cpacr;
    uint64_t bits;

    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    if (((cpacr >> 20) & 3) != 3) {
        return false;
    }

    __asm__ volatile(
        "fmov d0, #1.0\n"
        "fadd d0, d0, d0\n"
        "fmov %0, d0\n"
        : "=r"(bits) : : "d0");

    return bits == 0x4000000000000000UL;    /* 2.0 as an IEEE-754 double */
}

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

    __asm__ volatile("mov %0, sp" : "=r"(before));

    r = setjmp(test_jmp);
    if (r == 0) {
        jump_from_a_deep_frame();
    }

    __asm__ volatile("mov %0, sp" : "=r"(after));

    return r == 9 && after == before;
}

static const struct test tests[] = {
    { "boot: .bss is zeroed",                  test_bss_zeroed          },
    { "boot: .bss bounds are 16-byte aligned", test_bss_bounds_aligned  },
    { "boot: running at EL1",                  test_running_at_el1      },
    { "boot: sp is inside the boot stack",     test_sp_inside_boot_stack },
    { "trap: an undefined instruction faults", test_undefined_instruction_faults },
    { "trap: brk reports EC 0x3c",             test_brk_faults },
    { "trap: elr is the faulting instruction", test_elr_points_at_the_faulting_instruction },
    { "trap: execution resumes after a fault", test_execution_resumes_after_an_expected_fault },
    { "trap: no fault means end() is false",   test_unexpected_fault_is_not_swallowed },
    { "trap: the kernel runs on SP_EL0",       test_kernel_runs_on_sp_el0 },
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
    { "el0: a null deref kills only it",       test_a_null_dereference_kills_only_the_process },
    { "el0: it cannot read the kernel",        test_a_process_cannot_read_the_kernel },
    { "el0: it cannot write its own code",     test_a_process_cannot_write_its_own_code },
    { "el0: a syscall refuses a kernel ptr",   test_a_syscall_refuses_a_kernel_pointer },
    { "el0: separate address spaces",          test_processes_have_separate_address_spaces },
    { "as: a new space contains the kernel",   test_a_new_space_contains_the_kernel },
    { "as: map and unmap",                     test_a_space_maps_and_unmaps },
    { "as: the kernel region is refused",      test_a_space_refuses_the_kernel_region },
    { "as: switching makes a mapping real",    test_switching_to_a_space_makes_its_mapping_real },
    { "as: destroy returns its pages",         test_destroying_a_space_returns_its_pages },
    { "sched: a spinning thread is preempted", test_a_thread_that_never_yields_is_preempted },
    { "sched: both sides keep running",        test_preemption_does_not_lose_the_preempted_thread },
    { "ipc: call and reply",                   test_ipc_call_and_reply },
    { "ipc: both arrival orders work",         test_ipc_works_in_both_arrival_orders },
    { "ipc: destroy wakes the blocked",        test_destroying_an_endpoint_wakes_the_blocked },
    { "ipc: a stale capability fails",         test_a_stale_capability_fails },
    { "ipc: an out-of-range index fails",      test_a_capability_index_out_of_range_fails },
    { "ipc: endpoints are reclaimed",          test_endpoints_are_reclaimed },
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
    { "fp: EL1 may use FP and SIMD",           test_fp_is_usable_at_el1 },
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
    { "math: ours and newlib's agree",         test_our_math_matches_its_definition },
    { "lua: arithmetic, 2+2 is 4",             test_lua_arithmetic },
    { "lua: floats parse and print",           test_lua_floats },
    { "lua: strings and tables",               test_lua_strings_and_tables },
    { "lua: closures keep their upvalues",     test_lua_closures },
    { "lua: coroutines yield and resume",      test_lua_coroutines },
    { "lua: pcall catches errors",             test_lua_errors_are_caught },
    { "lua: nested pcall unwinds correctly",   test_lua_errors_nest },
    { "lua: the math library",                 test_lua_math_library },
    { "lua: the collector reclaims memory",    test_lua_garbage_collector_reclaims },
    { "lua: io, os, debug are absent",         test_lua_dangerous_libraries_are_absent },
    { "lua: load refuses bytecode",            test_lua_refuses_bytecode },
    { "sys: the kernel is inspectable",        test_sys_reports_the_kernel },
    { "sys: a spawned Lua thread does IPC",    test_a_spawned_lua_thread_answers_over_ipc },
    { "sys: destroy reaches a Lua server",     test_destroying_an_endpoint_reaches_a_lua_server },
    { "sys: IPC errors reach Lua",             test_lua_ipc_errors_are_reported },
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

void tests_run(void)
{
    unsigned failed = 0;

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

    semihosting_exit(failed == 0 ? 0 : 1);
}
