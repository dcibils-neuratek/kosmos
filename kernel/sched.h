/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
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

    /*
     * This thread is runnable and wants a turn, on processor `cpu`.
     *
     * **The queue is named rather than implied, and that is the whole of
     * what SMP changed here.** There is one runqueue per processor, so
     * "enqueue" is not a complete instruction without saying which - and the
     * caller is often not the core the thread will run on. A wake from core
     * zero's timer puts a thread on the queue of whichever core owns it.
     *
     * The caller holds that queue's lock. It is not taken here, because the
     * caller frequently wants two of these operations to be one atomic step
     * - `thread_yield` picks a successor and enqueues itself, and a policy
     * that locked internally would let another core see the queue in between
     * with neither thread in it.
     */
    void (*enqueue)(unsigned cpu, struct thread *t);

    /* The thread that should run now on processor `cpu`, removed from its
     * queue, or NULL when nothing is waiting there. The caller holds the
     * lock. */
    struct thread *(*pick_next)(unsigned cpu);

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
    bool (*ready)(unsigned cpu);

    /*
     * One timer tick has elapsed while `running` was on the CPU. Returns
     * true when the policy wants it replaced.
     *
     * This *is* acted on. `thread_tick` records the answer and the vector's
     * epilogue calls `thread_preempt_if_needed`, which does the switch -
     * the switch cannot happen in C inside the handler, because it moves
     * the stack the handler is standing on, and everything below that point
     * is still reading the trap frame off it.
     * The comment here used to say nothing acted on it, which stopped being
     * true when preemption landed and was not corrected at the time.
     */
    bool (*tick)(struct thread *running);

    /*
     * `woken` has just become runnable while `running` holds the CPU.
     * Should it take it now?
     *
     * Optional: a policy that leaves this NULL never preempts on a wake and
     * behaves exactly as before, which is what round robin does.
     *
     * This is the difference between a system that answers input in a
     * quantum and one that answers it in a switch. It cannot do the switch
     * itself - `thread_wake` is called from inside IPC and from the
     * interrupt path, and a switch there would move the stack out from
     * under whatever is reading the trap frame. It sets the same flag the
     * timer
     * sets, and the vector's epilogue acts on it.
     */
    bool (*preempts)(const struct thread *running, const struct thread *woken);
};

/* How many levels there are. Five would do; eight leaves room and costs
 * eight pointers. */
#define SCHED_PRIORITIES  8

/*
 * The levels that have names. Anything not named is `SCHED_PRIO_NORMAL`.
 *
 *   IDLE     runs only when nothing else will. The idle thread.
 *   LOW      background work nobody is waiting for.
 *   NORMAL   everything, unless there is a reason.
 *   DISPLAY  the compositor: what the eye is waiting for.
 *   AUDIO    a thread that renders or mixes sound: what the ear is
 *            waiting for, on a deadline the device sets every period.
 *            Asked for by the thread itself, allowed only to a process
 *            granted it, and held to a budget (below).
 *   INPUT    whoever reads the keyboard and the pointer. `design.md` and
 *            `ui.md` have both called this non-negotiable since before
 *            there was a scheduler that could express it.
 */
#define SCHED_PRIO_IDLE     0u
#define SCHED_PRIO_LOW      1u
#define SCHED_PRIO_NORMAL   2u
#define SCHED_PRIO_DISPLAY  3u
#define SCHED_PRIO_AUDIO    4u
#define SCHED_PRIO_INPUT    5u

/*
 * **Sound above every program, and the budget that makes that safe**
 * (`roadmap.md` 4i, step c). Diego, 28 September 2026: "audio should be
 * prioritized", "and not be jerky under heavy load" - and chose a band of
 * its own with a budget, over a thread taking its creator's band.
 *
 * Every program sits in the display band, because the screen is granted to
 * all of them, so a thread that renders sound for one was either in that
 * band - taking 100 ms turns with every busy window - or below it, where
 * anything drawing starves it outright. The audio server was put at DISPLAY
 * rather than above its clients for one reason, written down in
 * `process_grant_audio`: a band that high is safe only while the thread
 * blocks, and nothing enforced that it did. This is the enforcement.
 *
 * **A thread in the audio band that runs for longer than the budget without
 * sleeping drops back** to the band it had before it asked, and returns to
 * the audio band the next time it is woken. The budget is longer than a
 * whole ring takes to play - eight periods, 46 ms - so a thread that has
 * run that long without once sleeping cannot be keeping a ring and is not
 * doing a sound thread's work, and a real one, which renders a period in a
 * fraction of its length and sleeps, never meets it. Counted in the ticks
 * that land while the thread runs, so it is coarse by a tick, which is the
 * resolution the scheduler has.
 */
#define SCHED_AUDIO_BUDGET_TICKS  ((TICK_HZ + 19u) / 20u)

extern const struct scheduler sched_priority;

/* The quantum, in timer ticks. One tick is the floor; see sched_prio.c. */
void     sched_set_quantum(unsigned ticks);
/*
 * The longest turn a thread may be given, in scheduler ticks.
 *
 * `SYS_SCHED_SET` is deliberately unprivileged - changing the quantum is
 * tuning the machine you are sitting at, and on a single-user system there
 * is nobody to defend it from. That stays defensible only while the worst
 * an unprivileged caller can do is make the machine feel wrong. Four
 * billion ticks is four hundred days: threads in a band stop rotating and
 * the caller never gives the processor back, which is not tuning.
 *
 * A hundred ticks is one second at TICK_HZ, twice the largest the settings
 * app offers and far longer than any desktop wants.
 */
#define SCHED_QUANTUM_MAX   100

unsigned sched_get_quantum(void);

/*
 * The policies this machine has, and swapping between them while it runs.
 *
 * `sched_use` is the boot-time one and empties the queues; `sched_switch_to`
 * drains the runnable threads out of the old policy into the new one first,
 * which is the difference between changing your mind and losing every
 * thread on the machine.
 */
unsigned    sched_policy_count(void);
const char *sched_policy_name(unsigned index);
unsigned    sched_policy_index(void);
bool        sched_switch_to(unsigned index);

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
