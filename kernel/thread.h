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
 * Scheduling is cooperative today. A thread runs until it calls
 * `thread_yield` or blocks, and blocking is what IPC does, which is what
 * drives every switch once there are servers. Timer preemption needs the
 * switch to happen inside the interrupt handler's epilogue rather than in C,
 * and that is a separate piece of work: the groundwork is here, in that each
 * thread already owns both of its stacks.
 *
 * *Which* thread runs next is not decided here. That is the policy, and it
 * lives behind `struct scheduler` in `sched.h`, so a different algorithm is
 * a new file rather than an edit to this one.
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

/* Gives up the CPU to the next runnable thread. Returns when this thread is
 * scheduled again. With nothing else runnable it returns immediately. */
void thread_yield(void);

/* Takes the current thread off the runqueue and switches away. It will not
 * run again until something calls thread_wake on it. */
void thread_block(void);

/* Puts a blocked thread back on the runqueue. Safe to call on a thread that
 * is already runnable. */
void thread_wake(struct thread *t);

/* Ends the calling thread. Never returns. Reached automatically when a
 * thread's entry function returns. */
void thread_exit(void) __attribute__((noreturn));

struct thread *thread_current(void);
unsigned thread_count(void);

#endif /* KERNEL_THREAD_H */
