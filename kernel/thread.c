/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "kernel.h"
#include "percpu.h"
#include "smp.h"
#include "spinlock.h"
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

/*
 * This processor's own state, and the one slot there is.
 *
 * `NR_CPUS` is 1 and `percpu_init` claims entry zero at boot. Everything
 * that was a file-scope global here and is really a property of *a core*
 * lives in it now - `kernel/percpu.h` says which and why, and says what it
 * costs to have had them here.
 */
static struct percpu cpus[NR_CPUS];

struct percpu *this_cpu(void)
{
    return cpu_self();
}

struct percpu *percpu_at(unsigned index)
{
    return (index < NR_CPUS) ? &cpus[index] : NULL;
}

void percpu_init(unsigned index)
{
    cpus[index].index = index;
    cpu_set_self(&cpus[index]);
}

/*
 * The thread running on a given processor, which is not always this one.
 *
 * **It exists because `current` below is a macro**, expanding to
 * `this_cpu()->current` - so writing `p->current` for another core's slot
 * expands into nonsense that does not compile, which is the good outcome.
 * This function is deliberately placed *above* that definition, where the
 * field can still be named.
 *
 * One caller: `thread_wake`, deciding whether a thread it has just made
 * runnable outranks whatever is running on the core it will run on. That
 * decision used to read `current` and was therefore about the wrong
 * processor entirely.
 */
struct thread *percpu_running(struct percpu *p)
{
    return (p != NULL) ? p->current : NULL;
}

/*
 * The thread executing right now. Never NULL after thread_init.
 *
 * **A macro over the field, rather than twenty-nine edited call sites.**
 * They were correct and they say what they mean; rewriting every one to
 * `this_cpu()->current` would be a large diff whose entire content is a
 * change of spelling, and a large diff is where a real change hides. Linux
 * spells it the same way for the same reason.
 */
#define current     (this_cpu()->current)

/*
 * The installed policy. One of them, for every CPU: the queues it keeps are
 * indexed by core - `head[NR_CPUS][SCHED_PRIORITIES]` - so the vtable takes
 * a `cpu` and the policy is shared while its state is not.
 *
 * This comment used to say "one of them, for one CPU ... when SMP arrives
 * at M6 it becomes one instance per core", which was the intention before
 * `docs/smp.md` step five did it. It did not become one instance per core;
 * it became one instance with per-core state, which is the cheaper shape
 * and the one the vtable already allowed.
 */
static const struct scheduler *policy;

static unsigned next_id = 1;

/* Which processor the next thread created will call home. See where it is
 * used for what this is and is not. */
static unsigned next_cpu;

/*
 * Set by thread_tick inside the interrupt handler, acted on by
 * thread_preempt_if_needed in the vector's epilogue.
 *
 * The two are separate because the handler is the wrong place to switch and
 * the epilogue is the wrong place to make a policy decision. Splitting them
 * is what lets the decision be a C function the policy owns and the switch be
 * four instructions of assembly at a point where the stack is known.
 */
/* Per core: it is a statement about *this* core's return path. `smp.md`
 * listed six things that had to move and missed this one. */

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
 * One lock per processor's runqueue.
 *
 * **Held by the caller and never by the policy**, and never across
 * `context_switch`. Both halves of that matter.
 *
 * The caller holds it because a yield is two operations that must look like
 * one: pick a successor, then put the current thread back. A policy that
 * locked inside each call would expose an instant with neither thread in the
 * queue, and another core picking then would find it empty and idle while
 * two threads were runnable.
 *
 * It is not held across the switch because `context_switch` returns *on a
 * different stack* - the lock would be taken by one thread and released by
 * another, which is not a lock. So: take it, decide, let go, then switch.
 * That is an ordering constraint disguised as an architecture detail, and it
 * is the reason every site below looks the shape it does.
 *
 * Named the same on every core; the panic prints the holder's index, which
 * is what identifies which one. Initialised in `thread_init` rather than
 * statically because C11 has no way to write "this initialiser, N times"
 * without a GNU extension.
 */
static struct spinlock runq_lock[NR_CPUS];

/* This core's queue, which is what almost every caller wants. */
static inline unsigned here(void)
{
    return this_cpu()->index;
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
    unsigned long irqstate;

    if (index >= sched_policy_count()) {
        return false;
    }

    next = policies[index];

    if (next == policy) {
        return true;
    }

    /*
     * **Refused once more than one processor schedules, and that is a real
     * loss recorded as one.**
     *
     * Swapping the policy means draining every runnable thread out of the
     * old one and pouring it into the new. With one runqueue that is a local
     * operation. With one per core it is *every* core's queue at once - this
     * core would have to hold all of them simultaneously, which makes this
     * the single operation that defines a global lock order, and it would
     * have to stop the other cores mid-decision to do it.
     *
     * The honest answer is not to make that work but to say it does not.
     * Switching schedulers at runtime is a thing this system can do because
     * the policy is a vtable, and it exists to demonstrate that the
     * mechanism and the policy are genuinely separable - which it has done.
     * It is not a thing anybody needs while the machine is running four
     * cores' worth of work.
     *
     * `sysinfo` still reports the policy, `sched` still lists them, and the
     * suite still swaps them - because the suite runs with one scheduling
     * core. The day a secondary takes work, this returns false and says why.
     */
    if (thread_cpu_count() > 1) {
        return false;
    }

    /*
     * Masked for the duration, and holding this core's queue. A tick landing
     * between the drain and the install would ask a policy that owns half
     * the runnable threads which one runs next.
     */
    irqstate = spin_lock(&runq_lock[here()]);

    while (n < THREAD_MAX) {
        struct thread *t = policy->pick_next(here());

        if (t == NULL) {
            break;
        }

        drained[n++] = t;
    }

    policy = next;
    policy->init();

    for (i = 0; i < n; i++) {
        policy->enqueue(here(), drained[i]);
    }

    spin_unlock(&runq_lock[here()], irqstate);

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
/*
 * The thread pool's lock.
 *
 * **It covers the scan and the claim together, and it has to.** `alloc_thread`
 * used to return a slot and leave the caller to fill it in, which is a
 * window: two cores scanning at once both see the same `THREAD_UNUSED` slot,
 * both are handed it, and the second quietly overwrites the first's thread -
 * its stacks, its capability table, its id. Nothing faults. There is simply
 * one thread where there were two, and the one that vanished was in the
 * middle of something.
 *
 * So the claim happens *inside* this function, under the lock, before it
 * returns. The state it writes is `THREAD_CLAIMED`, which exists only to be
 * neither of the two states the scan looks for.
 */
static struct spinlock threads_lock = SPINLOCK("threads");


static struct thread *alloc_thread(void)
{
    unsigned long flags = spin_lock(&threads_lock);
    unsigned i;

    for (i = 0; i < THREAD_MAX; i++) {
        if (threads[i].state == THREAD_UNUSED) {
            threads[i].state = THREAD_CLAIMED;
            spin_unlock(&threads_lock, flags);
            return &threads[i];
        }
    }

    for (i = 0; i < THREAD_MAX; i++) {
        if (threads[i].state == THREAD_DEAD) {
            threads[i].state = THREAD_CLAIMED;
            spin_unlock(&threads_lock, flags);
            return &threads[i];
        }
    }

    spin_unlock(&threads_lock, flags);
    return NULL;
}

/*
 * A slot claimed and then not wanted, given back.
 *
 * `thread_create_suspended` can fail after `alloc_thread` succeeds - it runs
 * out of stacks - and a claimed slot that is never filled in is a slot lost
 * for the life of the machine. It was not a leak before this change only
 * because nothing marked the slot at all.
 */
static void release_thread(struct thread *t)
{
    unsigned long flags = spin_lock(&threads_lock);

    t->state = THREAD_DEAD;     /* not UNUSED: its stacks may exist */
    spin_unlock(&threads_lock, flags);
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
    unsigned c;

    for (c = 0; c < NR_CPUS; c++) {
        runq_lock[c].locked = 0;
        runq_lock[c].holder = SPIN_NOBODY;
        runq_lock[c].name   = "runqueue";
    }

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

    /* The boot thread is core zero's, and becomes its idle thread. */
    t->sched.cpu = 0;

    current = t;

    /*
     * And one for every other processor, reserved here and adopted there.
     *
     * A secondary's idle thread is the same shape as this one: not created,
     * but the code already running becoming a thread. It cannot adopt a slot
     * for itself, because `alloc_thread` walks a pool that no lock protects
     * and two cores arriving at once would take the same one - so core zero
     * reserves them all now, while it is the only processor in the machine,
     * and each secondary picks up the one with its own index.
     *
     * `THREAD_RUNNING` rather than a state of their own, and that is what
     * keeps them reserved: `alloc_thread` hands out only `THREAD_UNUSED` and
     * `THREAD_DEAD` slots, so a running one is invisible to it. They are
     * running - or will be, in a few hundred microseconds - which makes this
     * the true state rather than a convenient one.
     *
     * Stacks are NULL for the reason thread zero's are: they came from
     * `boot/start.S` rather than the page allocator, and nothing may ever
     * hand them back.
     */
    for (unsigned i = 1; i < NR_CPUS; i++) {
        struct thread *idle = &threads[i];

        idle->state = THREAD_RUNNING;
        idle->id = i;
        strcpy(idle->name, "idle");
        idle->name[4] = (char)('0' + i);
        idle->name[5] = '\0';
        idle->sched.next = NULL;
        idle->process = NULL;
        idle->space = NULL;
        idle->stack = NULL;
        idle->exception_stack = NULL;

        /* Its own core's, by definition: an idle thread is never enqueued
         * anywhere, but the field should say what is true rather than
         * default to zero and mean core zero. */
        idle->sched.cpu = i;

        percpu_at(i)->idle_thread = idle;
    }
}

/*
 * This processor's idle thread, adopted by the code that is running.
 *
 * Called once by each secondary as it comes up. Everything it needs was
 * reserved by core zero in `thread_init`; all this does is point `current`
 * at it, which is what makes `thread_current` answer on this core and what
 * lets `thread_tick` tell idle from busy here.
 *
 * It runs no other thread. There is one runqueue and it is core zero's, so
 * a secondary has nothing to switch to - `docs/smp.md` step five is what
 * changes that, and it needs the locks step two skipped.
 */
void thread_adopt_idle_here(void)
{
    struct thread *idle = this_cpu()->idle_thread;

    if (idle == NULL) {
        panic("thread: this processor was given no idle thread");
    }

    current = idle;
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
            release_thread(t);
            return NULL;
        }

        exception_top = alloc_stack(&t->exception_stack);
        if (exception_top == NULL) {
            /* The first stack is deliberately not handed back. Freeing it
             * would mean re-mapping its guard page, and this path only
             * happens when memory has already run out, where leaking four
             * pages matters far less than a half-undone unmap. */
            release_thread(t);
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
     * A context that has never run, built by the architecture rather than
     * here. What this function knows is that a thread is a function, an
     * argument and two stacks; which register each of those goes in is the
     * one thing about a thread that is not the same on every machine.
     */
    context_init(&t->ctx, entry, arg, stack_top, exception_top);

    /*
     * Under the pool's lock, because two cores creating a thread at the same
     * instant would otherwise be handed the same id - and an id is what
     * `procs`, `kill` and every capability check name a thread by.
     */
    {
        unsigned long idflags = spin_lock(&threads_lock);

        t->id = next_id++;

        /*
         * And where it will live, which is the whole of placement.
         *
         * **Round robin over the processors that schedule**, assigned once
         * and never revisited. Today `thread_cpu_count` is one, so every
         * thread comes home to core zero and the machine behaves exactly as
         * it did - which is what makes this safe to put in before the thing
         * it is for.
         *
         * It is deliberately the dumbest policy that is not obviously wrong.
         * Anything cleverer needs to know something this kernel cannot see
         * yet: which threads talk to each other (put them together, or the
         * IPC crosses cores), which are compute-bound (spread them), and on
         * a machine with unequal cores, which kind each one wants.
         * `docs/targets.md` has such a laptop in it. The right time to
         * choose is when there is something to measure.
         *
         * Under the pool lock because `next_cpu` is a counter two cores
         * would otherwise increment together - the same reason the id is
         * here, and free once the lock is already held.
         */
        t->sched.cpu = next_cpu % thread_cpu_count();
        next_cpu++;

        spin_unlock(&threads_lock, idflags);

    }
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
 * A thread that runs on a processor of the caller's choosing.
 *
 * **The only way anything crosses a core today**, and it exists because the
 * alternative - spreading every new thread automatically - has one thing
 * left in its way, which `thread_cpu_count` names.
 *
 * The home is set between creating and waking, which is the only window
 * where it can be: before creation there is no thread, and after waking it
 * is already on a queue and moving it would be migration.
 *
 * Everything a cross-core thread depends on is exercised by using this
 * once - placement, the target core's runqueue and its lock, the IPI that
 * wakes it out of `wfi`, and the idle loop that picks it up. That is the
 * whole of `docs/smp.md` steps five and six on one thread, which is what
 * the suite does with it.
 */
struct thread *thread_create_on(unsigned cpu, const char *name,
                                void (*entry)(void *), void *arg)
{
    struct thread *t;

    if (cpu >= smp_online()) {
        return NULL;
    }

    t = thread_create_suspended(name, entry, arg);

    if (t != NULL) {
        t->sched.cpu = cpu;
        thread_wake(t);
    }

    return t;
}

/*
 * Hands the CPU to `next`, from `prev`.
 *
 * The one place a switch happens. thread_exit used to have its own copy of
 * this and the two drifted: this one learned to switch the address space and
 * that one did not, so a process exiting left the page table root on the
 * kernel's tables
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
/*
 * Per core, and written out rather than hidden behind a macro.
 *
 * `current` gets one because it has twenty-nine call sites and because
 * `this_cpu()->current` at every one of them would be a diff whose only
 * content is spelling. These three have a dozen between them, and the macro
 * bought a trap instead: `cpus[index].idle_ticks` in `thread_load_cpu`
 * expanded to `cpus[index].(this_cpu()->idle_ticks)` and would not compile.
 *
 * A macro that shadows a *field name* is only safe while nothing indexes
 * the array by hand, which is exactly what the per-CPU work is going to do
 * more of, not less.
 */

void thread_set_idle(struct thread *t)
{
    this_cpu()->idle_thread = t;

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

/*
 * How many processors new threads are spread across.
 *
 * **The mechanism and the policy are different questions, and this is the
 * policy.** Every processor that arrived can run threads - `thread_create_on`
 * puts one on any of them and it does. This says how many `thread_create`
 * spreads across by default, and it exists as a variable because the answer
 * is not the same for every image.
 *
 * `smp_online()` for a machine somebody uses. **One for the test image**,
 * set by the suite before it runs anything, and that is not a hedge - it is
 * the honest reading of what those tests ask. A dozen of them mask
 * interrupts, create three threads and drive them by yielding, which is a
 * question about *this* processor's scheduler and only means anything if
 * those threads are here. Spreading them would not make the tests better,
 * it would make them stop asking. The checks that are about SMP say so by
 * using `thread_create_on`.
 */
static unsigned placement_cores;

void thread_place_across(unsigned cores)
{
    if (cores == 0) {
        cores = 1;
    }

    placement_cores = (cores > NR_CPUS) ? NR_CPUS : cores;
}

unsigned thread_cpu_count(void)
{
    /*
     * **How many processors new threads are spread across.** Three numbers
     * are easy to confuse here and all three exist:
     *
     *   `NR_CPUS`      how many `struct percpu` slots exist - the room, so
     *                  a core that starts has somewhere to put itself. Four.
     *   `smp_online()` how many have executed kernel code. Four.
     *   this           how many are given work. One, unless asked otherwise.
     *
     * Returning `NR_CPUS` here once said "4 scheduling" on a machine where
     * three cores were parked in `wfi`, which is why they are kept apart.
     *
     * Every processor that reached `secondary_main` has its own runqueue,
     * its own idle thread and its own timer, and runs the same loop core
     * zero runs. So the mechanism is finished and this is the *policy*;
     * `thread_place_across` sets it and `SMPWORK` reaches that from the
     * command line.
     *
     * **Work spreads now**: six compute-bound processes on four processors
     * read 100% on every core. It did not until the two faults in the
     * preemption path were fixed, and both are in this file - `thread_tick`
     * returned before `policy->tick` on every core but zero, and
     * `thread_wake` decided preemption about the waking core rather than
     * the target. Placement was never the fault; a trace showed the six
     * going to cpu 0,1,2,3,0,1 all along.
     *
     * **It is still one by default because of a failure that survived
     * both**: under `SMPWORK=4` the display harness does not get past its
     * editor phase. The desktop comes up and runs; the program typed into
     * `edit` does not come back.
     *
     * **This comment used to say the blocker was the four virtio drivers
     * having no locks, and said it in two contradictory blocks stacked on
     * top of each other.** They were locked in 0.9.20. `docs/smp.md` has
     * the measurement and what was ruled out to reach it.
     */
    /*
     * Zero means "however many are online", asked *now* rather than latched.
     *
     * It was latched on the first call, and the first call happens inside
     * `thread_init` - which runs before `smp_start_others`, when
     * `smp_online()` is still one. So the machine cached the answer from
     * before the other processors existed and placed every thread on core
     * zero for ever, while reporting "1 runs threads" on a screen showing
     * four. A cache whose first read is guaranteed to be wrong.
     */
    if (placement_cores != 0) {
        return placement_cores;
    }

    return smp_online();
}

void thread_load_cpu(unsigned index, unsigned long *idle, unsigned long *busy)
{
    if (index >= NR_CPUS) {
        *idle = 0;
        *busy = 0;
        return;
    }

    *idle = cpus[index].idle_ticks;
    *busy = cpus[index].busy_ticks;
}

/*
 * The machine's total, which is the sum and not this core's.
 *
 * It read `this_cpu()->idle_ticks` - a macro over *this* processor's - which is the
 * same number on a machine with one core and quietly the wrong one on any
 * other. Written as a sum now, while the loop runs once and the mistake is
 * still free to fix.
 */
void thread_load(unsigned long *idle, unsigned long *busy)
{
    unsigned i;

    *idle = 0;
    *busy = 0;

    for (i = 0; i < NR_CPUS; i++) {
        *idle += cpus[i].idle_ticks;
        *busy += cpus[i].busy_ticks;
    }
}

void thread_tick(void)
{
    /* Before thread_init has run there is nothing to preempt, and the timer
     * starts before the first thread exists. */
    if (current == NULL || policy->tick == NULL) {
        return;
    }

    if (current == this_cpu()->idle_thread) {
        this_cpu()->idle_ticks++;
    } else {
        this_cpu()->busy_ticks++;
    }

    /*
     * **Machine-wide work, and only core zero may do it.**
     *
     * `thread_wake_sleepers` scans the whole of `threads[]`. Four cores
     * doing that at TICK_HZ each would be four scans a tick finding the
     * same deadlines, and the `wake_at` it clears is not claimed under any
     * lock - so two cores can both decide the same sleeper is due. One core
     * owning the scan is the cheap answer and there is no reason to want
     * another.
     *
     * First, so a thread whose deadline has arrived is runnable before any
     * core's policy decision below is made rather than one tick later.
     */
    if (this_cpu()->index == 0) {
        thread_wake_sleepers();
    }

    /*
     * **And everything below is this processor's, which is the fix.**
     *
     * This was behind the `index != 0` return above, and the comment there
     * explained why that was safe: "a secondary runs only its own idle
     * thread, so there is no thread of its own to preempt". It called
     * itself "a temporary invariant, named so it is found when the locks
     * arrive". The locks arrived at step two, the runqueues at step five,
     * and nothing came back here.
     *
     * **What it cost is that only core zero ever preempted on a quantum.**
     * A compute-bound thread on cores 1-3 could not be taken off by the
     * timer at all: it ran until it blocked or exited. With placement on,
     * that is most of the machine.
     *
     * It is safe on every core because neither line touches anything
     * shared. `current` is `this_cpu()->current`, so `ticks++` is this
     * core's own thread; and both policies' `tick` do nothing but decrement
     * `running->sched.quantum` on the thread handed to them. The switch it
     * asks for is not taken here either - `preempt_pending` is this core's
     * flag, read by this core's exception epilogue, which is the one place
     * where moving the stack is safe.
     */
    current->ticks++;

    if (policy->tick(current)) {
        this_cpu()->preempt_pending = true;
    }
}

void thread_preempt_if_needed(void)
{
    struct thread *next;

    if (!this_cpu()->preempt_pending) {
        return;
    }

    this_cpu()->preempt_pending = false;

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

    /*
     * Decide under the lock, switch outside it.
     *
     * `context_switch` returns on another stack, so a lock held across it
     * would be released by a different thread than took it. Everything that
     * touches the queue happens between these two lines and nothing else
     * does.
     */
    {
        unsigned      cpu   = here();
        unsigned long flags = spin_lock(&runq_lock[cpu]);

        next = policy->pick_next(cpu);

        if (next != NULL) {
            policy->enqueue(cpu, current);
        }

        spin_unlock(&runq_lock[cpu], flags);
    }

    if (next == NULL) {
        return;     /* nothing else wants the CPU */
    }

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
    unsigned      cpu;
    unsigned long flags;
    bool          any;

    if (policy->ready == NULL) {
        return false;
    }

    /*
     * This core's queue, and only this core's.
     *
     * The idle loop asks so that it knows whether it may sleep, and the
     * answer it needs is about work *it* can run. Another core having a
     * runnable thread is not a reason for this one to stay awake - that
     * thread has a home and this is not it.
     */
    cpu = here();
    flags = spin_lock(&runq_lock[cpu]);
    any = policy->ready(cpu);
    spin_unlock(&runq_lock[cpu], flags);

    return any;
}

void thread_yield(void)
{
    unsigned long  irqstate;
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
    irqstate = cpu_interrupts_save();

    /*
     * Ask before offering. Picking first and enqueuing afterwards is what
     * makes this a yield rather than a no-op: enqueuing the caller first
     * would let a FIFO policy hand it straight back.
     *
     * **The lock is the other half of what the mask above buys.** Masking
     * stops a tick on *this* core landing in the window; the lock stops
     * another core scheduling in it. Both are needed and neither replaces
     * the other - a mask is a statement about one processor.
     *
     * `spin_unlock` restores the interrupt state `spin_lock` found, which is
     * already masked because of the save above, so the mask survives to the
     * switch and is put back by the caller's `cpu_interrupts_restore`.
     */
    {
        unsigned      cpu   = here();
        unsigned long flags = spin_lock(&runq_lock[cpu]);

        next = policy->pick_next(cpu);

        if (next != NULL) {
            policy->enqueue(cpu, current);
        }

        spin_unlock(&runq_lock[cpu], flags);
    }

    if (next != NULL) {
        switch_to(next);
    }

    /*
     * Reached when this thread runs again. `context_switch` restored this
     * thread's own saved mask on the way back in, and this puts back what
     * the caller had.
     */
    cpu_interrupts_restore(irqstate);
}

/*
 * Block, and let go of a lock the caller was holding, at the one instant
 * where letting go is safe.
 *
 * --------------------------------------------------------------------
 * Why this exists
 * --------------------------------------------------------------------
 *
 * IPC has an ordering problem that only appears with two cores. `ipc_call`
 * hands the message to the receiver and *then* puts itself on the endpoint's
 * `awaiting_reply` list, records `waiting_on`, and blocks. On one core that
 * is fine, because nothing runs in between. On two, the receiver can run on
 * another core and reply before the sender is on the list at all - and
 * `ipc_reply` then finds no peer and returns an error, or worse, finds a
 * sender whose state is still THREAD_RUNNING and whose wake is therefore
 * dropped on the floor. That does not corrupt anything. It silently loses a
 * message, which is harder to find.
 *
 * The fix is to hold the endpoint's lock from before the hand-off until
 * after this thread is *both* findable and blocked - and those two facts
 * become true at different moments, which is why the release has to happen
 * in here rather than at the call site.
 *
 * --------------------------------------------------------------------
 * Why it can release before the switch, which is the interesting part
 * --------------------------------------------------------------------
 *
 * The textbook answer is that the lock must be handed to the *next* thread
 * and released on the far side of `context_switch` - Linux does exactly
 * that. The reason is a genuine race: this thread is marked blocked and is
 * findable, so another core can wake it and a third can resume it, on the
 * stack this core is still saving.
 *
 * **That race cannot happen here, and the reason is placement.** A thread
 * has a home core, assigned when it is created, and never migrates. So a
 * wake can only ever enqueue it on *its own* core's runqueue, and only that
 * core picks from that queue - and that core is this one, which is busy
 * inside the switch. By the time this core looks at its queue again the
 * context is long saved.
 *
 * So the simple thing is correct, and it is correct *because* of a decision
 * made somewhere else. If work stealing is ever added - or migration, or a
 * balancer - this comment is where the cost shows up: the release would have
 * to move to the far side of the switch, and every entry into a thread,
 * including a brand new one starting at its trampoline, would have to know
 * about it.
 */
void thread_block_and_release(struct spinlock *lock, unsigned long flags)
{
    struct thread *next;
    unsigned       cpu;
    unsigned long  rq;

    cpu = here();
    rq = spin_lock(&runq_lock[cpu]);

    current->state = THREAD_BLOCKED;
    next = policy->pick_next(cpu);

    spin_unlock(&runq_lock[cpu], rq);

    /*
     * Now, and not before: this thread is on the caller's list *and* is
     * blocked, so a waker on another core finds it and the wake sticks.
     */
    if (lock != NULL) {
        spin_unlock(lock, flags);
    }

    /*
     * Nothing to run here means idle, not deadlock.
     *
     * **That panic was a statement about the machine and is now a statement
     * about one processor.** With a single runqueue, "nothing is runnable"
     * really did mean every thread in the system was blocked with nobody
     * left to wake anybody - a deadlock, and saying so beat hanging. With a
     * queue per core it means this core has nothing to do, which is the
     * ordinary state of an idle processor and happens constantly.
     *
     * So it falls back to this core's idle thread, which is what an idle
     * thread is for. It was the first thing to break when placement was
     * turned on: three cores went to the panic within a second of getting
     * their first thread.
     */
    if (next == NULL) {
        next = this_cpu()->idle_thread;
    }

    if (next == NULL || next == current) {
        panic("thread_block: this processor has no idle thread");
    }

    switch_to(next);
}

void thread_block(void)
{
    struct thread *next;
    unsigned       cpu;
    unsigned long  flags;

    /*
     * The state change and the pick under one lock.
     *
     * **They have to be one step.** `docs/smp.md`'s audit found this: if
     * `state = THREAD_BLOCKED` is visible before a successor is chosen,
     * another core's timer can see a blocked thread whose `wake_at` has
     * expired, mark it READY and enqueue it - while this core is still
     * inside `switch_to` running on that thread's stack. A third core then
     * picks it up and resumes it, on a stack somebody is using.
     *
     * The switch itself is outside, because a lock cannot be held across
     * `context_switch`.
     */
    cpu = here();
    flags = spin_lock(&runq_lock[cpu]);

    current->state = THREAD_BLOCKED;
    next = policy->pick_next(cpu);

    spin_unlock(&runq_lock[cpu], flags);

    /* This core's idle thread when its queue is empty; see
     * `thread_block_and_release` for why that is not a deadlock any more. */
    if (next == NULL) {
        next = this_cpu()->idle_thread;
    }

    if (next == NULL || next == current) {
        panic("thread_block: this processor has no idle thread");
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
/*
 * How fast the counter counts, asked once.
 *
 * Cached rather than passed in at boot, so there is no initialisation order
 * to get right: the first sleep asks, and every one after it is a load.
 * `cpu_identify` is a register read on one board and a CPUID on the other,
 * and neither belongs on a path a syscall takes.
 *
 * The fallback is what `sysinfo` reports when the board could not say -
 * never zero, because the division below would be one.
 */
static uint64_t counter_hz(void)
{
    static uint64_t hz;

    if (hz == 0) {
        struct cpu_info cpu;

        cpu_identify(&cpu);
        hz = cpu.counter_hz != 0 ? cpu.counter_hz : 62500000UL;
    }

    return hz;
}

uint64_t thread_deadline_in(unsigned long ticks)
{
    return cpu_cycles() + (uint64_t)ticks * counter_hz() / TICK_HZ;
}

void thread_sleep_until(uint64_t deadline)
{
    if (deadline <= cpu_cycles()) {
        return;
    }

    current->wake_at = deadline;
    thread_block();
    current->wake_at = 0;
}

void thread_wait_input_until(uint64_t deadline)
{
    if (deadline <= cpu_cycles()) {
        return;
    }

    current->wake_at       = deadline;
    current->wake_on_input = true;
    thread_block();
    current->wake_on_input = false;
    current->wake_at       = 0;
}

void thread_wake_sleepers(void)
{
    uint64_t now = cpu_cycles();
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

        /* `wake_on_input`, not merely `wake_at`. A thread that asked to
         * sleep is not a thread waiting for a key, and waking it here is
         * what made every sleep on x86 last one tick. */
        if (t->state == THREAD_BLOCKED && t->wake_on_input) {
            t->wake_at       = 0;
            t->wake_on_input = false;
            thread_wake(t);
        }
    }
}

void thread_wake(struct thread *t)
{
    /*
     * **This look is a hint. The decision is made again under the lock**,
     * fifteen lines down, and the difference between those two sentences
     * was three cores going idle with runnable threads on them.
     *
     * `t->state` is read here with nothing held, and a thread becomes
     * BLOCKED only under `runq_lock[t->sched.cpu]` - so between this test
     * and that lock the answer can change, and when two cores are waking
     * the same thread they can both be told yes. Both then enqueued it.
     *
     * What that does to the queue is worse than it sounds, because
     * `prio_enqueue` begins by writing `t->sched.next = NULL`. Enqueuing a
     * thread that is already *in* the list therefore cuts the list at that
     * thread while `tail` still points past the cut: everything after it
     * becomes unreachable from `head` and is never scheduled again. The
     * queue then drains to empty, `occupied` clears, and the core parks in
     * `wfi` holding threads that are ready to run and can no longer be
     * found.
     *
     * It is not subtle when it happens. Six spinners placed across four
     * processors: within a second, three of the four read 0% and stay
     * there, and the machine that was using four cores is using one. The
     * desktop showed the same thing more quietly - one bar that would not
     * rise - and the display harness showed it as an editor that never ran
     * the program typed into it.
     *
     * The second waker is real rather than theoretical, and `ipc.c` is
     * where it comes from: `deliver` clears `waiting_on` and then wakes,
     * while core zero's tick is inside `thread_wake_sleepers` for the same
     * thread, and `ipc_timed_out` returns immediately once `waiting_on` is
     * NULL without ever taking the endpoint lock. Nothing serialises the
     * two, and nothing did.
     *
     * The fast path is kept because it is worth keeping: almost every call
     * here is a wake of a thread that really is blocked, and the ones that
     * are not - a wake racing a wake - are exactly the ones that must not
     * take the cheap answer.
     */
    if (t->state == THREAD_BLOCKED) {
        /*
         * Onto **its** queue, which is not necessarily this core's.
         *
         * A thread has a home - `t->sched.cpu`, set when it was created -
         * and a wake can come from anywhere: core zero's timer walking the
         * sleepers, an IPC reply on whichever core the server ran on, an
         * input interrupt. So this is the one place where a core routinely
         * takes *another* core's lock, and it is why the lock is per queue
         * rather than per core in the sense of "the lock this core uses".
         *
         * One lock, not two: nothing here touches this core's queue, so
         * there is no ordering between two queue locks to get wrong. That is
         * a property worth keeping - the moment work stealing arrives it
         * stops being true, which is one of the reasons work stealing is
         * not here.
         */
        unsigned      cpu   = t->sched.cpu;
        unsigned long flags = spin_lock(&runq_lock[cpu]);

        /*
         * And now, holding the lock that guards the transition, ask again.
         *
         * Whoever else was waking this thread either has not started or has
         * finished; if they finished, it is READY or RUNNING by now and this
         * call has nothing left to do. Returning here rather than falling
         * through also skips the poke and the preemption check below, which
         * is right: the core has already been told, by the waker that won.
         */
        if (t->state != THREAD_BLOCKED) {
            spin_unlock(&runq_lock[cpu], flags);
            return;
        }

        t->state = THREAD_READY;
        policy->enqueue(cpu, t);

        spin_unlock(&runq_lock[cpu], flags);

        /*
         * And if it lives somewhere else, tell that processor to look.
         *
         * **Outside the lock, and after the enqueue**, both deliberately.
         * After, because the poke is only meaningful once the thread is
         * findable - the barrier inside `hal_cpu_wake` is what makes that
         * ordering visible to the other core rather than merely written
         * here. Outside, because the target takes the interrupt immediately
         * and its first act is to want this same lock.
         *
         * Without this the wake is not lost, it is late: the target notices
         * at its own next tick, up to four milliseconds away. For a
         * background thread that is nothing. For IPC it is everything - a
         * shell command is dozens of round trips, and a tick each way would
         * make four processors slower than one, which is the way this whole
         * exercise could have been got exactly wrong.
         *
         * Not sent to this core. An interrupt to oneself is a wasted
         * exception: the epilogue on the way out of whatever is running
         * already asks the scheduler, which is the thing the poke exists to
         * make happen.
         */
        /*
         * **And if it outranks whoever is running *there*, say so there.**
         *
         * This is the second half of the same bug as the tick above, and it
         * is the one that mattered for IPC. The comparison below used to
         * read `current` - which is `this_cpu()->current`, the thread on the
         * *waking* core - and set `this_cpu()->preempt_pending`, the
         * *waking* core's flag. Neither has anything to do with the core the
         * woken thread lives on.
         *
         * So a cross-core wake preempted the wrong processor, or more often
         * nothing at all: it compared a woken thread against a thread it
         * will never compete with, and flagged a core that is not going to
         * run it. The woken thread then waited for the target's own quantum
         * to expire - which, before the fix above, never happened on a
         * secondary.
         *
         * Both halves are read through `percpu_at(cpu)` now, which is what
         * that function exists for.
         *
         * Enqueuing alone means the woken thread waits for the running one's
         * quantum to expire - up to a hundred milliseconds, for a thread the
         * policy considers more important. That is the whole of why an input
         * event could sit behind a compute-bound one.
         *
         * The switch is not done here on either core. `thread_wake` is
         * called from inside IPC and from the interrupt path, and switching
         * there would move the stack while something above is still reading
         * the trap frame at `sp`. Setting the flag the timer already sets
         * means the vector's epilogue does it, at the one place where it is
         * safe - and on the target core that is exactly what the IPI below
         * brings about.
         *
         * The band comparison is done here, before the policy is asked. IPC
         * wakes a thread on every message and almost always wakes a peer -
         * a server and its client both run at NORMAL - so the indirect call
         * through `preempts` was paid on every round trip to be told "no".
         * Comparing the two numbers first settles the common case without a
         * call. It assumes a larger band outranks a smaller one, which is a
         * property of `SCHED_PRIO_*` in sched.h rather than a secret of any
         * one policy; the call below still has the final say.
         */
        {
            struct percpu *target  = percpu_at(cpu);
            struct thread *running = percpu_running(target);

            if (running != NULL && t != running
                && t->sched.effective > running->sched.effective
                && policy->preempts != NULL && policy->preempts(running, t)) {
                target->preempt_pending = true;
            }
        }

        /*
         * And then tell that processor to look, which is what turns the flag
         * above into a switch: the target takes the interrupt, and the
         * epilogue on its way out asks the scheduler.
         *
         * **After the flag, and the barrier inside `hal_cpu_wake` is what
         * makes that ordering visible to the other core** rather than merely
         * written in this order here. Outside the queue lock, because the
         * target's first act on taking the interrupt is to want it.
         *
         * Not sent to this core. An interrupt to oneself is a wasted
         * exception: the epilogue on the way out of whatever is running here
         * already asks the scheduler.
         */
        if (cpu != here()) {
            hal_cpu_wake(cpu);
        }
    }
}

/*
 * The FP registers may still be attributed to a thread that is going away.
 * From each board's `fp.c`; saving into a slot that is about to be recycled
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

    /*
     * Dead and a successor chosen, under one lock, for the reason
     * `thread_block` gives: a state visible before the pick lets another
     * core act on this thread while it is still running on its own stack.
     */
    {
        unsigned      cpu   = here();
        unsigned long flags = spin_lock(&runq_lock[cpu]);

        current->state = THREAD_DEAD;
        next = policy->pick_next(cpu);

        spin_unlock(&runq_lock[cpu], flags);
    }

    /* The same fallback `thread_block_and_release` explains: a core with an
     * empty queue idles rather than panicking. */
    if (next == NULL) {
        next = this_cpu()->idle_thread;
    }

    if (next == NULL || next == current) {
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
