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
 * Sixteen until M6, which was a number and not a derivation: chosen at M3
 * when the system had three threads and never revisited. Forty-eight now,
 * and this is where the sizing is written down.
 *
 * What a slot costs, measured rather than estimated:
 *
 *   struct thread   2,640 bytes of .bss per slot, always, used or not. Most
 *                   of it is the 2 KB `struct message` embedded below: a
 *                   thread's message in flight lives on the thread, so the
 *                   kernel never has to allocate one.
 *   its stacks      40 KB of RAM per *live* thread - two stacks of four
 *                   pages, each with a guard page allocated and then
 *                   unmapped - taken from the page allocator at creation
 *                   and given back when the thread dies.
 *
 * So the pool is cheap and the threads are not, which is the right way round
 * for a limit: forty-eight slots is 127 KB of .bss and nothing more until
 * something actually runs.
 *
 * **Why forty-eight.** Every process has a thread and PROCESS_MAX is 32, so
 * thirty-three is the floor: the boot thread and one per process. The rest
 * is room for a process to have more than one thread, which nothing does yet
 * and the app server will.
 */
struct memobj;

#define CAP_NONE      0
#define CAP_ENDPOINT  1
#define CAP_MEMORY    2

#define THREAD_MAX          48
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

    /* Timer ticks charged to this thread. Only rises; a percentage is the
     * difference between two readings, for the same reason the machine-wide
     * figure is. */
    unsigned long ticks;

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
        /*
         * Which band this thread runs in: `SCHED_PRIO_*` in sched.h.
         * Set to NORMAL at creation, because zero is the idle band and a
         * zeroed struct would put everything there.
         *
         * This is the band it was *given*. What it actually runs at is
         * `thread_effective_priority`, which is this or a band borrowed
         * from whoever is waiting on it, whichever is higher.
         */
        unsigned       priority;

        /*
         * A band borrowed from a caller, or zero.
         *
         * Priority inheritance, and the reason it exists here: a server
         * runs at the priority of whoever is blocked waiting for it. Without
         * it, the console server cannot be given the input band - it is also
         * the path every `print` takes, so at the top band it outranks
         * everything it serves and starves the machine, which is exactly
         * what happened when that promotion was tried.
         *
         * With it the question does not arise. The server sits at NORMAL and
         * *becomes* urgent for exactly as long as something urgent is
         * waiting on it, which is the honest answer to "how important is
         * this server" - it depends entirely on who is asking.
         *
         * This is QNX's mechanism and it fits a synchronous rendezvous
         * exactly: the kernel already knows who is blocked on whom, because
         * that is what `ipc_call` is.
         */
        unsigned       inherited;
        uint64_t       key;
        unsigned long  quantum;
    } sched;

    /*
     * When this thread asked to be woken, in counter ticks, or zero.
     *
     * A thread that has nothing to do should not be running, and until now
     * there was no way for it to say so: everything here polled. The window
     * manager cannot block on a message, because the thing it is mostly
     * waiting for is a key, and a key is not a message - so it looped, and
     * one thread that never stops being runnable is enough to keep a core
     * at a hundred per cent for ever.
     *
     * Checked on the timer tick, which is the only clock there is.
     */
    unsigned long     wake_at;

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
    /*
     * A capability names one of two kinds of thing now: an endpoint, or a
     * region of memory two processes share. The kind is stored rather than
     * inferred, so a slot holding one can never be read as the other -
     * which is the mistake a union without a tag invites and which would be
     * a process handing out a pointer to somebody's pixels as a place to
     * send messages.
     *
     * The generation is checked for both, against the same hazard: a slot
     * whose object was destroyed and replaced.
     */
    struct {
        unsigned char    kind;      /* CAP_NONE, CAP_ENDPOINT, CAP_MEMORY */
        struct endpoint *endpoint;
        struct memobj   *memory;
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

/* Whether any thread other than the running one is waiting for a turn.
 * For the idle loop, which must not sleep while one is. */
bool thread_any_ready(void);

/* Takes the current thread off the runqueue and switches away. It will not
 * run again until something calls thread_wake on it. */
void thread_block(void);

/*
 * Blocks until `deadline` (a value of hal_ticks) or until something else
 * wakes this thread, whichever comes first. A deadline already past
 * returns at once.
 */
void thread_sleep_until(unsigned long deadline);

/* Wakes every sleeper whose deadline has arrived. Called from the tick. */
void thread_wake_sleepers(void);

/* Wakes every sleeper regardless of deadline. For an interrupt that is the
 * thing they were waiting for. */
void thread_wake_sleepers_now(void);

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
 * Which band a thread runs in. Takes effect at its next turn: a thread
 * already on a queue stays where it was enqueued, which is one quantum of
 * imprecision and not worth a requeue to remove.
 */
void thread_set_priority(struct thread *t, unsigned priority);
unsigned thread_priority(const struct thread *t);

/*
 * What a thread actually runs at: its own band, or one borrowed from a
 * caller, whichever is higher. Everything that schedules asks this rather
 * than reading `priority`.
 */
unsigned thread_effective_priority(const struct thread *t);

/*
 * `to` is about to work on `from`'s behalf, so it runs at `from`'s band
 * until it replies. Chains: a server that calls another server passes on
 * whatever it is carrying, because this reads the effective band.
 */
void thread_inherit(struct thread *to, const struct thread *from);

/* Finished on somebody's behalf; back to its own band. */
void thread_disinherit(struct thread *t);

/*
 * Ticks spent idle and ticks spent working, since boot. Both only rise: a
 * percentage is the difference between two readings, which is the only kind
 * that can mean "recently".
 */
void thread_load(unsigned long *idle, unsigned long *busy);

/* How many capabilities a thread holds. What a process may reach is exactly
 * this many things, and no others. */
unsigned thread_cap_count(const struct thread *t);

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
