#ifndef KERNEL_SCHED_H
#define KERNEL_SCHED_H

#include <stdbool.h>

struct thread;

/*
 * The scheduling policy, behind an interface.
 *
 * `thread.c` owns the mechanism: stacks, contexts, state transitions and the
 * switch itself. This owns the one question mechanism cannot answer, which
 * is *which* runnable thread should run next. Keeping the two apart means a
 * different algorithm is a new file rather than an edit to the thread code,
 * and it makes the choice measurable: swap the policy, run the same
 * benchmark, compare.
 *
 * Everything a policy needs to store lives in `struct thread`'s `sched`
 * member. There is no allocation here and there cannot be, per CLAUDE.md:
 * a policy that needs per-thread state uses those fields, and a policy that
 * needs global state declares it statically in its own file.
 *
 * The contract:
 *
 *   - A thread is in the queue, or it is running, never both. `pick_next`
 *     removes what it returns.
 *   - `enqueue` is only ever called on a thread that is runnable.
 *   - A policy must not switch threads, block, or touch a context. It
 *     chooses; it does not act.
 */
struct scheduler {
    const char *name;

    /* Called once, before any thread exists. */
    void (*init)(void);

    /* This thread is runnable and wants a turn. */
    void (*enqueue)(struct thread *t);

    /* The thread that should run now, removed from the queue, or NULL when
     * nothing is waiting. */
    struct thread *(*pick_next)(void);

    /*
     * One timer tick has elapsed while `running` was on the CPU. Returns
     * true when the policy wants it replaced.
     *
     * Nothing acts on that answer yet: switching threads from inside an
     * interrupt handler needs the switch to happen in the vector's epilogue
     * rather than in C, which is its own piece of work. The hook is here
     * because a preemptive policy is unwritable without it, and because
     * every thread already owns both of its stacks, which is the part that
     * would have been expensive to retrofit.
     */
    bool (*tick)(struct thread *running);
};

/*
 * Installs a policy. Legal only before `thread_init`, or from a test that
 * knows exactly which threads exist: swapping policies with threads queued
 * would strand them in the outgoing policy's structures.
 */
void sched_use(const struct scheduler *policy);
const struct scheduler *sched_current(void);

/* The policies that exist. */
extern const struct scheduler sched_round_robin;

#endif /* KERNEL_SCHED_H */
