#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "thread.h"
#include "pmm.h"
#include "page.h"
#include "panic.h"
#include "hal.h"
#include "console.h"
#include "ipc.h"
#include "mmu.h"
#include "sched.h"

/*
 * The pool. Statically declared and never grown, which removes a whole class
 * of bug in one stroke: there is no allocation to fail halfway, no lifetime
 * to get wrong, and a thread pointer is valid for as long as the kernel is.
 */
static struct thread threads[THREAD_MAX];

/* The thread executing right now. Never NULL after thread_init. */
static struct thread *current;

/*
 * The installed policy. One of them, for one CPU: the queue it keeps is a
 * per-CPU structure in everything but name, and when SMP arrives at M6 it
 * becomes one instance per core rather than being redesigned. CLAUDE.md asks
 * for that shape from the start.
 */
static const struct scheduler *policy;

static unsigned next_id = 1;

/*
 * Set by thread_tick inside the interrupt handler, acted on by
 * thread_preempt_if_needed in the vector's epilogue.
 *
 * The two are separate because the handler is the wrong place to switch and
 * the epilogue is the wrong place to make a policy decision. Splitting them
 * is what lets the decision be a C function the policy owns and the switch be
 * four instructions of assembly at a point where the stack is known.
 */
static volatile bool preempt_pending;

/* The effective band, worked out in one place; see below. */
static void refresh_effective(struct thread *t);

void sched_use(const struct scheduler *p)
{
    policy = p;
    policy->init();
}

/*
 * Every policy this machine has, in the order a chooser shows them.
 *
 * A list rather than a compile-time choice because the whole point of
 * having the seam is being able to feel the difference, and a difference
 * you have to rebuild to see is one nobody looks at.
 */
static const struct scheduler *const policies[] = {
    &sched_priority,
    &sched_round_robin,
};

unsigned sched_policy_count(void)
{
    return (unsigned)(sizeof(policies) / sizeof(policies[0]));
}

const char *sched_policy_name(unsigned index)
{
    return index < sched_policy_count() ? policies[index]->name : NULL;
}

unsigned sched_policy_index(void)
{
    unsigned i;

    for (i = 0; i < sched_policy_count(); i++) {
        if (policies[i] == policy) {
            return i;
        }
    }

    return 0;
}

/*
 * Change policy with threads already queued in the old one.
 *
 * `sched_use` calls `init`, which empties the queues - correct at boot,
 * where nothing is in them, and a way to lose every runnable thread on the
 * machine at any other time. The threads are not in a list the kernel keeps;
 * they are in whatever structure the policy chose, and the only handle on
 * them is `pick_next`.
 *
 * So they are drained one at a time out of the old policy and handed to the
 * new one, before the new one is installed. Interrupts are masked for the
 * duration: a tick landing halfway through would ask a policy that owns half
 * the runnable threads which one should run next.
 */
bool sched_switch_to(unsigned index)
{
    const struct scheduler *next;
    struct thread *drained[THREAD_MAX];
    unsigned n = 0;
    unsigned i;
    unsigned long daif;

    if (index >= sched_policy_count()) {
        return false;
    }

    next = policies[index];

    if (next == policy) {
        return true;
    }

    /*
     * Masked for the duration. A tick landing between the drain and the
     * install would ask a policy that owns half the runnable threads which
     * one runs next. Saved and restored rather than unconditionally
     * re-enabled, so this is correct when called from somewhere that
     * already held them off.
     */
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #3" ::: "memory");

    while (n < THREAD_MAX) {
        struct thread *t = policy->pick_next();

        if (t == NULL) {
            break;
        }

        drained[n++] = t;
    }

    policy = next;
    policy->init();

    for (i = 0; i < n; i++) {
        policy->enqueue(drained[i]);
    }

    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");

    return true;
}

const struct scheduler *sched_current(void)
{
    return policy;
}

struct thread *thread_current(void)
{
    return current;
}

const struct thread *thread_by_index(unsigned i)
{
    if (i >= THREAD_MAX
        || threads[i].state == THREAD_UNUSED
        || threads[i].state == THREAD_DEAD) {
        return NULL;
    }

    return &threads[i];
}

/* Threads that could still run. A dead one is not counted: its slot is
 * reusable and counting it would make the number mean "slots touched"
 * rather than "threads alive". */
unsigned thread_count(void)
{
    unsigned n = 0;
    unsigned i;

    for (i = 0; i < THREAD_MAX; i++) {
        if (threads[i].state != THREAD_UNUSED
            && threads[i].state != THREAD_DEAD) {
            n++;
        }
    }

    return n;
}

/* strncpy with the truncation made explicit and the terminator guaranteed,
 * which is the part strncpy famously does not give you. */
static void copy_name(char *dst, const char *src)
{
    size_t i;

    for (i = 0; i + 1 < THREAD_NAME_MAX && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }

    dst[i] = '\0';
}

/*
 * A free slot: never used, or used by a thread that has since exited.
 *
 * Recycling a dead thread's slot also recycles its stacks, and that is the
 * point rather than a shortcut. They are already allocated with their guard
 * pages already unmapped, which is exactly the state a new thread wants;
 * handing them back to the page allocator would mean re-mapping those guards
 * only to unmap them again.
 *
 * A dead thread is not standing on its stack any more. It stopped the moment
 * thread_exit switched away, and it can never be scheduled again.
 *
 * Untouched slots are preferred so that reuse only starts once the pool is
 * genuinely full, which keeps a use-after-exit bug visible for as long as
 * possible rather than being masked by an immediate recycle.
 */
static struct thread *alloc_thread(void)
{
    unsigned i;

    for (i = 0; i < THREAD_MAX; i++) {
        if (threads[i].state == THREAD_UNUSED) {
            return &threads[i];
        }
    }

    for (i = 0; i < THREAD_MAX; i++) {
        if (threads[i].state == THREAD_DEAD) {
            return &threads[i];
        }
    }

    return NULL;
}

/*
 * A stack, with an unmapped page underneath it.
 *
 * The guard is why this asks the allocator for one page more than it needs
 * and then removes that page from the map. Without it a thread that
 * overflows quietly writes into whatever the page allocator handed out
 * before it, which is another thread's stack about as often as not.
 *
 * Returns the top, which is what a stack pointer wants.
 */
/* The top of a stack allocated by alloc_stack, from its base. */
static void *stack_top_of(void *base)
{
    return (char *)base + (THREAD_STACK_PAGES + 1) * PAGE_SIZE;
}

static void *alloc_stack(void **base_out)
{
    void *base = pmm_alloc_contiguous(THREAD_STACK_PAGES + 1);

    if (base == NULL) {
        return NULL;
    }

    /* The lowest page becomes the guard. Growing down, a thread reaches it
     * before it reaches anything belonging to somebody else. */
    mmu_unmap_page((uintptr_t)base);

    *base_out = base;
    return stack_top_of(base);
}

void thread_init(void)
{
    struct thread *t = &threads[0];

    /* Round robin unless a test or a boot option already chose otherwise. */
    if (policy == NULL) {
        sched_use(&sched_priority);
    }

    /*
     * Thread zero is not created, it is adopted: the code running right now
     * becomes it. Its context is filled in the first time it is switched
     * away from, so there is nothing to build here.
     *
     * Its stacks are the ones from the linker script rather than the page
     * allocator, which is why they are recorded as NULL: nothing may ever
     * hand them back.
     */
    t->state = THREAD_RUNNING;
    t->id = 0;
    strcpy(t->name, "boot");
    t->sched.next = NULL;   /* running, so not in the queue */
    t->process = NULL;
    t->space = NULL;
    t->stack = NULL;
    t->exception_stack = NULL;

    current = t;
}

struct thread *thread_create_suspended(const char *name,
                                      void (*entry)(void *), void *arg)
{
    struct thread *t = alloc_thread();
    void *stack_top;
    void *exception_top;

    if (t == NULL) {
        return NULL;
    }

    if (t->stack != NULL) {
        /* A recycled slot, with its stacks and their guard pages already in
         * place. Only the top of each has to be recomputed. */
        stack_top     = stack_top_of(t->stack);
        exception_top = stack_top_of(t->exception_stack);
    } else {
        stack_top = alloc_stack(&t->stack);
        if (stack_top == NULL) {
            return NULL;
        }

        exception_top = alloc_stack(&t->exception_stack);
        if (exception_top == NULL) {
            /* The first stack is deliberately not handed back. Freeing it
             * would mean re-mapping its guard page, and this path only
             * happens when memory has already run out, where leaking four
             * pages matters far less than a half-undone unmap. */
            return NULL;
        }
    }

    memset(&t->ctx, 0, sizeof(t->ctx));

    /*
     * Everything the previous occupant of this slot left behind.
     *
     * The capability table above all: a recycled thread inheriting the dead
     * one's capabilities would reach endpoints it was never handed, which is
     * the one thing capabilities exist to prevent. It would also be a
     * particularly quiet bug, since everything would appear to work.
     */
    memset(t->caps, 0, sizeof(t->caps));
    t->process = NULL;
    t->space = NULL;
    memset(&t->ipc, 0, sizeof(t->ipc));
    memset(&t->sched, 0, sizeof(t->sched));

    /*
     * Normal, not zero.
     *
     * Zeroing the whole struct is right for everything else in it and wrong
     * for this one field: level 0 is the idle band, so every thread created
     * would have been the least important thing on the machine and the
     * scheduler would have run them in the order they happened to be
     * enqueued - which looks exactly like round robin working, and is not.
     */
    t->sched.priority = SCHED_PRIO_NORMAL;
    t->sched.effective = SCHED_PRIO_NORMAL;

    /*
     * A context built by hand so the first `ret` in context_switch lands in
     * thread_entry, which reads the function out of x19 and its argument out
     * of x20. The thread has never run, so there is nothing else to restore.
     */
    t->ctx.x19 = (uint64_t)(uintptr_t)entry;
    t->ctx.x20 = (uint64_t)(uintptr_t)arg;
    t->ctx.x30 = (uint64_t)(uintptr_t)thread_entry;
    t->ctx.sp_el0 = (uint64_t)(uintptr_t)stack_top;
    t->ctx.sp_el1 = (uint64_t)(uintptr_t)exception_top;

    /*
     * A new thread starts on SP_EL0 with interrupts enabled. Both are zero
     * and the memset already produced them, but leaving either implicit
     * makes the first thread that starts on the wrong stack, or with
     * interrupts masked, a mystery rather than a line to read.
     */
    t->ctx.daif = 0;
    t->ctx.spsel = 0;

    t->id = next_id++;
    t->switches = 0;

    copy_name(t->name, name);

    /* Blocked rather than ready: the caller decides when it may run, which
     * is what lets it finish building whatever the thread will need. */
    t->state = THREAD_BLOCKED;

    return t;
}

struct thread *thread_create(const char *name, void (*entry)(void *), void *arg)
{
    struct thread *t = thread_create_suspended(name, entry, arg);

    if (t != NULL) {
        thread_wake(t);
    }

    return t;
}

/*
 * Hands the CPU to `next`, from `prev`.
 *
 * The one place a switch happens. thread_exit used to have its own copy of
 * this and the two drifted: this one learned to switch the address space and
 * that one did not, so a process exiting left TTBR0 on the kernel's tables
 * and whichever process ran next did so with somebody else's memory
 * underneath it. Duplicated control flow does not stay duplicated.
 */
static void switch_into(struct thread *prev, struct thread *next)
{
    next->state = THREAD_RUNNING;
    next->switches++;
    current = next;

    /*
     * The address space follows the thread. Safe to do here, from kernel
     * code, precisely because the kernel is mapped in every space: the
     * instruction after this one is fetched through the new tables and finds
     * itself where it was.
     */
    if (next->space != prev->space) {
        as_switch(next->space);
    }

    context_switch(&prev->ctx, &next->ctx);
}

static void switch_to(struct thread *next)
{
    struct thread *prev = current;

    if (next == prev) {
        return;
    }

    if (prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
    }

    switch_into(prev, next);

    /*
     * Reached when this thread is scheduled again, which may be a long time
     * later and from a different thread than the one it switched to. Nothing
     * below here may assume anything about `next`.
     */
}

/*
 * How busy the machine is.
 *
 * Sampled rather than accumulated: at every timer tick, whichever thread was
 * running gets the tick charged to it - to `idle` if it was the one that has
 * nothing to do, and to `busy` otherwise. A hundred samples a second is
 * plenty to say what fraction of the time the machine is working, and it
 * costs one comparison on the interrupt path.
 *
 * Accumulating real elapsed time per thread would be more precise and would
 * mean reading the counter twice on every context switch, which is a cost
 * paid on the hottest path in the kernel to answer a question nobody asks
 * more than once a second.
 *
 * Both counters only ever rise. Whoever wants a percentage takes two
 * readings and divides the difference, which is also the only way to get a
 * number that means "recently" rather than "since boot".
 */
static struct thread   *idle_thread;
static unsigned long    idle_ticks;
static unsigned long    busy_ticks;

void thread_set_idle(struct thread *t)
{
    idle_thread = t;

    /*
     * And into the band that only runs when nothing else will. Without
     * this the idle loop competes with real work at normal priority and
     * takes a turn in the rotation, which is not idling, it is spinning
     * politely.
     */
    if (t != NULL) {
        t->sched.priority = SCHED_PRIO_IDLE;
        refresh_effective(t);
    }
}

/*
 * The one place the effective band is worked out. Everything that changes
 * either input calls this; nothing else writes the field.
 */
static void refresh_effective(struct thread *t)
{
    t->sched.effective = t->sched.inherited > t->sched.priority
                       ? t->sched.inherited : t->sched.priority;
}

unsigned thread_effective_priority(const struct thread *t)
{
    return t == NULL ? SCHED_PRIO_NORMAL : t->sched.effective;
}

void thread_inherit(struct thread *to, const struct thread *from)
{
    unsigned band;

    if (to == NULL || from == NULL || to == from) {
        return;
    }

    band = thread_effective_priority(from);

    /*
     * Only ever upward, and only while it is higher than what the thread
     * already carries. A server serving two clients at once - which a
     * coroutine server does - keeps the more urgent of them, and `reply`
     * clears it rather than trying to work out which one just left. That is
     * imprecise for exactly one request and self-correcting on the next.
     */
    if (band > to->sched.inherited) {
        to->sched.inherited = band;
        refresh_effective(to);
    }
}

void thread_disinherit(struct thread *t)
{
    if (t != NULL) {
        t->sched.inherited = 0;
        refresh_effective(t);
    }
}

void thread_set_priority(struct thread *t, unsigned priority)
{
    if (t == NULL) {
        return;
    }

    if (priority >= SCHED_PRIORITIES) {
        priority = SCHED_PRIORITIES - 1;
    }

    t->sched.priority = priority;
    refresh_effective(t);
}

unsigned thread_cap_count(const struct thread *t)
{
    unsigned i, n = 0;

    if (t == NULL) {
        return 0;
    }

    for (i = 0; i < CAPS_PER_THREAD; i++) {
        if (t->caps[i].kind != CAP_NONE) {
            n++;
        }
    }

    return n;
}

void thread_load(unsigned long *idle, unsigned long *busy)
{
    *idle = idle_ticks;
    *busy = busy_ticks;
}

void thread_tick(void)
{
    /* Before thread_init has run there is nothing to preempt, and the timer
     * starts before the first thread exists. */
    if (current == NULL || policy->tick == NULL) {
        return;
    }

    if (current == idle_thread) {
        idle_ticks++;
    } else {
        busy_ticks++;
    }

    /* Before the policy is asked anything, so a thread whose deadline has
     * arrived is runnable when the decision is made rather than one tick
     * later. */
    thread_wake_sleepers();

    /* And to the thread itself, which is what makes a per-process figure
     * possible. One increment on the interrupt path; the alternative is
     * reading the counter on every context switch, which is the hottest
     * path in the kernel. */
    current->ticks++;

    if (policy->tick(current)) {
        preempt_pending = true;
    }
}

void thread_preempt_if_needed(void)
{
    struct thread *next;

    if (!preempt_pending) {
        return;
    }

    preempt_pending = false;

    if (current == NULL) {
        return;
    }

    /*
     * Only a thread that is still runnable may be preempted. One that
     * blocked inside the handler has already chosen its successor, and
     * putting it back on the queue here would make it runnable again with
     * nothing to wake it for.
     */
    if (current->state != THREAD_RUNNING) {
        return;
    }

    next = policy->pick_next();

    if (next == NULL) {
        return;     /* nothing else wants the CPU */
    }

    policy->enqueue(current);
    switch_to(next);
}

/*
 * Is there another thread that wants the CPU?
 *
 * Only the idle loop asks. See the comment on `ready` in sched.h for what
 * happens when the answer is not consulted.
 */
bool thread_any_ready(void)
{
    return policy->ready != NULL && policy->ready();
}

void thread_yield(void)
{
    unsigned long  daif;
    struct thread *next;

    /*
     * Masked for the duration, and this one was found the hard way.
     *
     * A syscall arrives with interrupts already masked, so `SYS_YIELD` was
     * safe. A *kernel* thread calling this is not: it runs with interrupts
     * enabled, and there is a window of three instructions below where a
     * timer tick corrupts the runqueue.
     *
     * The tick lands between the enqueue and the switch. `current` is on the
     * runqueue by then and its state is still THREAD_RUNNING, because
     * `switch_to` is what changes it - so `thread_preempt_if_needed`, whose
     * whole guard is that state, does not recognise the situation and
     * enqueues `current` a *second* time.
     *
     * `prio_enqueue` then does `tail[level]->sched.next = t` on a thread
     * already in that list. If it is the tail, `t->sched.next = t` - a
     * self-loop, so `head[level]` never empties, the occupancy bit never
     * clears, and every other thread in that band becomes unreachable. If it
     * is mid-list, `t->sched.next = NULL` truncates the list while
     * `tail[level]` still points past the end, and the next pick reads
     * through a NULL head.
     *
     * Which is exactly the two symptoms: a machine that hangs, and
     * `sched_prio.c` faulting on a NULL `head[level]` while the bitmask said
     * that band had somebody in it. Both were intermittent for the same
     * reason - the window is three instructions wide and needs a tick to
     * land inside it.
     *
     * Saved and restored rather than unconditionally re-enabled, so this
     * stays correct when called from somewhere that already held them off -
     * the same reason `sched_switch_to` does it that way.
     */
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #3" ::: "memory");

    /*
     * Ask before offering. Picking first and enqueuing afterwards is what
     * makes this a yield rather than a no-op: enqueuing the caller first
     * would let a FIFO policy hand it straight back.
     */
    next = policy->pick_next();

    if (next != NULL) {
        policy->enqueue(current);
        switch_to(next);
    }

    /*
     * Reached when this thread runs again. `context_switch` restored this
     * thread's own saved mask on the way back in, and this puts back what
     * the caller had.
     */
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
}

void thread_block(void)
{
    struct thread *next;

    current->state = THREAD_BLOCKED;
    next = policy->pick_next();

    if (next == NULL) {
        /*
         * Everything is blocked and nothing can wake anybody, because
         * waking happens in thread context and there is no thread left to
         * run. Until there are interrupts that wake threads, this is a
         * deadlock and saying so beats hanging.
         */
        panic("thread_block: every thread is blocked");
    }

    switch_to(next);
}

/*
 * Sleeping, and waking the sleepers.
 *
 * A deadline on the thread and a scan on the tick, rather than a sorted
 * queue: there are forty-eight slots, the scan is forty-eight comparisons a
 * hundred times a second, and a sorted structure would be more code to get
 * wrong than the thing it saves. If the pool ever grows enough for this to
 * matter, `sched.key` is already there for a deadline-ordered queue and this
 * is the function to replace.
 */
void thread_sleep_until(unsigned long deadline)
{
    if (deadline <= hal_ticks()) {
        return;
    }

    current->wake_at = deadline;
    thread_block();
    current->wake_at = 0;
}

void thread_wake_sleepers(void)
{
    unsigned long now = hal_ticks();
    unsigned i;

    for (i = 0; i < THREAD_MAX; i++) {
        struct thread *t = &threads[i];

        if (t->state == THREAD_BLOCKED && t->wake_at != 0
            && now >= t->wake_at) {
            t->wake_at = 0;

            /* If it was waiting on an endpoint rather than merely sleeping,
             * take it off that queue *before* it becomes runnable - see
             * `ipc_timed_out`. Harmless for a plain sleeper. */
            ipc_timed_out(t);
            thread_wake(t);
        }
    }
}

void thread_wake_sleepers_now(void)
{
    unsigned i;

    for (i = 0; i < THREAD_MAX; i++) {
        struct thread *t = &threads[i];

        if (t->state == THREAD_BLOCKED && t->wake_at != 0) {
            t->wake_at = 0;
            thread_wake(t);
        }
    }
}

void thread_wake(struct thread *t)
{
    if (t->state == THREAD_BLOCKED) {
        t->state = THREAD_READY;
        policy->enqueue(t);

        /*
         * And if it outranks whoever is running, say so.
         *
         * Enqueuing alone means the woken thread waits for the running
         * one's quantum to expire - up to a hundred milliseconds, for a
         * thread the policy considers more important. That is the whole of
         * why an input event could sit behind a compute-bound one.
         *
         * The switch is not done here. `thread_wake` is called from inside
         * IPC and from the interrupt path, and switching there would move
         * SP_EL1 while something above is still reading the trap frame at
         * `sp`. Setting the flag the timer already sets means the vector's
         * epilogue does it, at the one place where it is safe.
         */
        /*
         * The band comparison is done here, before the policy is asked.
         *
         * IPC wakes a thread on every message and almost always wakes a
         * peer - a server and its client both run at NORMAL - so the
         * indirect call through `preempts` was paid on every round trip to
         * be told "no". Comparing the two numbers first settles the common
         * case without a call.
         *
         * This does assume a larger band number outranks a smaller one,
         * which is a property of `SCHED_PRIO_*` in sched.h rather than a
         * secret of any one policy. A policy that disagrees is free to: the
         * call below still has the final say, and one that wants to preempt
         * a *peer* can say so by returning true - it just will not be asked
         * about a thread that ranks lower.
         */
        if (current != NULL && t != current
            && t->sched.effective > current->sched.effective
            && policy->preempts != NULL && policy->preempts(current, t)) {
            preempt_pending = true;
        }
    }
}

/*
 * The FP registers may still be attributed to a thread that is going away.
 * From arch/aarch64/fp.c; saving into a slot that is about to be recycled
 * is silent corruption the moment somebody else gets it.
 */
void fp_forget(struct thread *t);

void thread_abandon(struct thread *t)
{
    if (t == NULL || t == current || t->state != THREAD_BLOCKED) {
        panic("thread_abandon: not an unstarted thread");
    }

    /* Dead rather than unused, so the slot is recycled with its stacks
     * still allocated and their guard pages still unmapped, which is what
     * alloc_thread expects to find. */
    fp_forget(t);
    t->state = THREAD_DEAD;
}

void thread_exit(void)
{
    struct thread *next;

    fp_forget(current);
    current->state = THREAD_DEAD;
    next = policy->pick_next();

    if (next == NULL) {
        panic("thread_exit: the last thread returned");
    }

    /*
     * A dead thread is never enqueued again, so it leaves the policy's
     * structures by simply not going back in.
     *
     * Its stacks stay allocated, and stay with the slot: whoever reuses it
     * inherits them, guard pages and all. Handing them back would mean
     * re-mapping those guards only to unmap them again for the next thread.
     */
    switch_into(current, next);

    panic("thread_exit: a dead thread was scheduled");
}
