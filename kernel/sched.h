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
     * Is anybody waiting for a turn?
     *
     * `pick_next` answers this too, but answering it that way takes the
     * thread off the queue, and the one caller that needs to *ask* is the
     * idle loop - which wants to know whether it may sleep, not to run
     * something. Getting this wrong is expensive in a way that is invisible:
     * an idle thread that executes `wfi` while a thread is runnable makes
     * every yield cost a whole timer period, and every IPC round trip two
     * of them. That was twenty milliseconds a message here.
     */
    bool (*ready)(void);

    /*
     * One timer tick has elapsed while `running` was on the CPU. Returns
     * true when the policy wants it replaced.
     *
     * This *is* acted on. `thread_tick` records the answer and the vector's
     * epilogue calls `thread_preempt_if_needed`, which does the switch -
     * the switch cannot happen in C inside the handler, because it moves
     * SP_EL1 and everything below that point reads the trap frame at `sp`.
     * The comment here used to say nothing acted on it, which stopped being
     * true when preemption landed and was not corrected at the time.
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
