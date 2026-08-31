#ifndef KERNEL_THREAD_H
#define KERNEL_THREAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "context.h"
#include "ipc.h"

/*
 * Threads and the scheduler.
 *
 * A fixed pool, statically declared, per CLAUDE.md: there is no dynamic
 * allocator in the kernel and there will not be one. Running out of threads
 * is a `NULL` from `thread_create`, not a growing table.
 *
 * Scheduling is preemptive. A thread yields, blocks, or is taken off the CPU
 * by the timer when the policy says its turn is over. Blocking is what IPC
 * does and is what drives most switches once there are servers; preemption is
 * what stops a thread that does neither from owning the machine.
 *
 * *Which* thread runs next is not decided here. That is the policy, and it
 * lives behind `struct scheduler` in `sched.h`, so a different algorithm is
 * a new file rather than an edit to this one.
 */

/*
 * How many threads there can be, ever.
 *
 * A fixed pool because `CLAUDE.md` forbids an allocator in the kernel, and
 * the reason that rule is worth having is visible here: running out is an
 * error at a known limit, reported at the one call that asks for a thread,
 * rather than a failure at an unknown one somewhere later. There is no
 * fragmentation, nothing to audit, and no path where the kernel is halfway
 * through something when memory runs out.
 *
 * **Sixteen is a number, not a derivation.** It was chosen at M3 when the
 * system had three threads and has never been revisited; this comment exists
 * because it was the only constant in this file without one. What it costs,
 * measured rather than estimated:
 *
 *   struct thread   2,640 bytes of .bss per slot, always, whether or not
 *                   anything uses it. Sixteen of them is 41 KB.
 *   its stacks      40 KB of RAM per *live* thread - two stacks of four
 *                   pages, each with a guard page allocated and then
 *                   unmapped - taken from the page allocator at creation
 *                   and given back when the thread dies.
 *
 * So the pool is cheap and the threads are not, which is the right way round
 * for a limit: raising this costs 2.6 KB and nothing else until something
 * actually runs.
 *
 * **It is not the limit that binds.** Every process has a thread and
 * PROCESS_MAX is 8, so nine threads is the practical ceiling today - the
 * boot thread and one per process. The number to raise when the app server
 * arrives is that one, and this one after it.
 */
#define THREAD_MAX          16
#define THREAD_NAME_MAX     16

/* 16 KB each, matching the boot stack, plus a guard page below each. */
#define THREAD_STACK_PAGES  4

enum thread_state {
    THREAD_UNUSED = 0,
    THREAD_READY,       /* on the runqueue, waiting for a turn */
    THREAD_RUNNING,     /* the one executing right now */
    THREAD_BLOCKED,     /* waiting on something; not on the runqueue */
    THREAD_DEAD         /* returned from its entry function */
};

struct process;
struct addrspace;

struct thread {
    /* First, because switch.S reaches it through the thread pointer and a
     * zero offset is one instruction cheaper to think about. */
    struct context ctx;

    enum thread_state state;
    unsigned id;
    char name[THREAD_NAME_MAX];

    /*
     * Scheduler-private. The installed policy owns every field here and
     * nothing else may read or write them.
     *
     * They are embedded rather than reached through a pointer because there
     * is no allocator to hand a policy its own storage, and they are named
     * generally rather than for round robin so a different policy is a new
     * file and not a change to this struct. Between them they cover the
     * shapes the usual algorithms need: a link for any queue, a priority for
     * the priority-ordered ones, a key for anything sorted by virtual time
     * or deadline, and a quantum for the preemptive ones.
     */
    struct {
        struct thread *next;
        unsigned       priority;
        uint64_t       key;
        unsigned long  quantum;
    } sched;

    /*
     * IPC state.
     *
     * `next` here is a separate link from `sched.next` and must stay
     * separate. A thread blocked on an endpoint is in that endpoint's queue
     * and *not* in the scheduler's, so one field could not hold both; using
     * one would corrupt whichever queue was touched second, and it would do
     * it silently.
     */
    struct {
        struct message  msg;        /* the message in flight, either way */
        struct thread  *next;       /* link in an endpoint's wait queue */
        struct thread  *peer;       /* who sent to us, or who we sent to */
        struct endpoint *waiting_on;/* so destroying an endpoint can find us */
        int             status;     /* the result handed over on waking */
    } ipc;

    /*
     * Capabilities, by index. A thread cannot name what it was not handed:
     * there is no global table and no identifier to guess. At M4 this moves
     * to the process, which is where design.md puts it.
     */
    struct {
        struct endpoint *endpoint;
        unsigned         generation;
    } caps[CAPS_PER_THREAD];

    /*
     * The process this thread is running, or NULL in a kernel thread.
     *
     * Per thread and not a global, which was found the hard way. With one
     * process a global was indistinguishable from correct; with two, whichever
     * ran last owned it, so a syscall checked one process's pointers against
     * the other's address space and an exception from EL0 arrived with no
     * process at all.
     */
    struct process *process;

    /*
     * The address space this thread runs in, or NULL for the kernel's.
     *
     * Switched by switch_to, because it has to follow the thread. Setting
     * TTBR0 once when a process starts leaves whichever process ran last
     * owning the page tables, and the next one to be scheduled runs with
     * somebody else's memory underneath it: its own code reads another
     * process's data, and its own instructions are fetched from addresses
     * that mean something different.
     *
     * Kept here rather than reached through `process` so thread.c does not
     * have to know what a process is.
     */
    struct addrspace *space;

    /* Kept so the stacks can be handed back, and so a fault inside a thread
     * can name which one it was. */
    void *stack;
    void *exception_stack;

    unsigned long switches;     /* how many times this thread was resumed */
};

/* Installs the round-robin policy unless `sched_use` already chose another,
 * then turns the code currently executing into the first thread. Everything that
 * ran before this becomes thread 0, which is the one that boots the system
 * and, once there is nothing left to start, becomes the idle thread. */
void thread_init(void);

/* A new thread, ready to run. NULL when the pool is full or there are not
 * enough pages for its stacks. */
struct thread *thread_create(const char *name, void (*entry)(void *), void *arg);

/*
 * The same, but not yet runnable. `thread_wake` starts it.
 *
 * For anything that is not fully built by the time thread_create returns. A
 * thread is schedulable the instant it is created, and with preemption that
 * instant is real: a process whose capabilities were granted on the next
 * line had already run, found an empty table, and exited.
 */
struct thread *thread_create_suspended(const char *name,
                                       void (*entry)(void *), void *arg);

/* Gives up the CPU to the next runnable thread. Returns when this thread is
 * scheduled again. With nothing else runnable it returns immediately. */
void thread_yield(void);

/* Takes the current thread off the runqueue and switches away. It will not
 * run again until something calls thread_wake on it. */
void thread_block(void);

/* Puts a blocked thread back on the runqueue. Safe to call on a thread that
 * is already runnable. */
void thread_wake(struct thread *t);

/*
 * One timer tick has elapsed. Asks the policy whether the running thread
 * should be replaced and records the answer; it does not switch.
 *
 * Called from the interrupt handler, where switching is not allowed: the
 * switch has to happen in the vector's epilogue, on a frame that belongs to
 * whichever thread is about to be resumed.
 */
void thread_tick(void);

/*
 * Names the thread that runs when there is nothing to do, so that the tick
 * can tell working from waiting. Called once, by kmain, about itself.
 */
void thread_set_idle(struct thread *t);

/*
 * Ticks spent idle and ticks spent working, since boot. Both only rise: a
 * percentage is the difference between two readings, which is the only kind
 * that can mean "recently".
 */
void thread_load(unsigned long *idle, unsigned long *busy);

/*
 * Performs the switch thread_tick asked for, if it asked for one.
 *
 * Called only from the vector epilogue in vectors.S. Calling it from C would
 * move SP_EL1 out from under whatever frame the caller is standing on.
 */
void thread_preempt_if_needed(void);

/*
 * Releases a thread that was created suspended and never started.
 *
 * Not thread_exit: that ends the *calling* thread, which is the right thing
 * when a thread finishes and precisely the wrong thing when somebody is
 * cleaning up after a thread that never ran. The slot and its stacks go back
 * to the pool the same way an exited thread's do.
 */
void thread_abandon(struct thread *t);

/* Ends the calling thread. Never returns. Reached automatically when a
 * thread's entry function returns. */
void thread_exit(void) __attribute__((noreturn));

struct thread *thread_current(void);

/*
 * The thread in slot `i`, or NULL when nothing there could still run.
 *
 * A dead thread is skipped, for the same reason `thread_count` does not
 * count one: its slot is reusable, so listing it would be reporting a
 * recyclable slot as an inhabitant. The two views have to agree, or a caller
 * that uses both sees a system that contradicts itself.
 *
 * For inspection only. This is how `sys.threads` and, at M5, `/proc`
 * enumerate them, and nothing may hold the pointer across a yield.
 */
const struct thread *thread_by_index(unsigned i);
unsigned thread_count(void);

#endif /* KERNEL_THREAD_H */
