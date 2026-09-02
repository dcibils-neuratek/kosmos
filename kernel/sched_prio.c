/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Priorities, with round robin inside each one.
 *
 * Round robin alone is fair and that is all it is. Fairness is the wrong
 * goal for the thing this system is actually trying to be: `design.md` and
 * `ui.md` have both said since the beginning that input runs at the highest
 * priority and called it non-negotiable, and for a year neither was true,
 * because `sched_rr.c` had nothing to prioritise with and said so in its
 * own first comment. This is that sentence made real.
 *
 * **What is borrowed from QNX, and what is not.** QNX is a microkernel with
 * the same shape as this one - threads, address spaces, synchronous
 * messages, everything else in userland - and it is a real-time system,
 * which does not mean fast. It means *bounded*: the highest-priority ready
 * thread starts running within a known worst case, every time. Two of the
 * things that buy that are worth having here and cost almost nothing:
 *
 *   * **strict priority with immediate preemption.** A thread that becomes
 *     ready and outranks the running one does not wait for a quantum to
 *     expire. That is `preempts` below, and it is the whole latency story:
 *     the quantum decides how long a *peer* waits, and preemption decides
 *     how long a superior waits. Ours waited up to a hundred milliseconds.
 *
 *   * **a quantum that can be changed and measured**, rather than a number
 *     compiled in and never questioned.
 *
 * What is deliberately not borrowed: two hundred and fifty six levels,
 * where five say everything this system has to say; and hard guarantees,
 * which would mean bounding every kernel operation and is a promise nothing
 * here needs. Kosmos wants a desktop that feels alive, not an airbag that
 * fires in time.
 *
 * **Starvation is real and is accepted, with eyes open.** Strict priority
 * means a busy high thread can hold the CPU forever and the levels below it
 * never run. QNX answers with adaptive partitioning; the answer here is
 * that the only threads above `PRIO_NORMAL` are the ones that read input
 * and put pixels on the screen, and both block almost immediately. If that
 * stops being true, this comment is where to start.
 */

#include <stddef.h>

#include "sched.h"
#include "thread.h"
#include "panic.h"

/*
 * How many ticks a thread gets before its turn is over.
 *
 * Ten at the 100 Hz the timer runs at, so 100 ms, which is the value round
 * robin used and is kept so that this change is about priorities and not
 * about two things at once.
 *
 * It is a variable rather than a constant because the interesting thing
 * about a quantum is what happens when you change it, and a system you can
 * only ask that of by rebuilding is one nobody asks.
 *
 * **The floor is one tick.** A quantum is counted in timer interrupts, so
 * 10 ms is as fine as this gets without raising `TICK_HZ` - and BeOS, whose
 * feel this is chasing, ran a quantum of a few milliseconds. Going below
 * 10 ms here is a change to the timer, not to this number, and it costs an
 * interrupt per millisecond to get there.
 */
static unsigned quantum_ticks = 10;

/* One queue per level. Level 0 runs only when nothing else wants to. */
static struct thread *head[SCHED_PRIORITIES];
static struct thread *tail[SCHED_PRIORITIES];

/*
 * Which levels have anybody in them, one bit each.
 *
 * The obvious implementation walks the levels from the top until it finds a
 * queue that is not empty, and the obvious implementation cost 43% of a
 * context switch and 32% of an IPC round trip - because almost everything
 * runs at NORMAL, so almost every pick scanned the five empty bands above
 * it first. The benchmarks caught it immediately, which is the argument for
 * having had them since M3.
 *
 * A bitmask makes the same question one instruction: the highest set bit is
 * the highest occupied level, and `__builtin_clz` compiles to `clz`. Both
 * QNX and Linux keep exactly this, for exactly this reason.
 */
static unsigned occupied;

/*
 * A thread's level. Read, not validated.
 *
 * The only two ways the field is ever written are thread creation, which
 * sets NORMAL, and `thread_set_priority`, which clamps - so it is in range
 * by construction and the clamp that used to be here was paid on every
 * enqueue for a case that cannot happen. That cost showed up as a whole
 * tick on `context_switch`, which is what benchmarking a scheduler change
 * is for.
 *
 * If a third writer ever appears, it clamps too, or this becomes an index
 * off the end of two arrays.
 */
static inline unsigned level_of(const struct thread *t)
{
    return t->sched.priority;
}

static void prio_init(void)
{
    unsigned i;

    for (i = 0; i < SCHED_PRIORITIES; i++) {
        head[i] = NULL;
        tail[i] = NULL;
    }

    occupied = 0;
}

/* The highest level with anybody in it, or SCHED_PRIORITIES when empty. */
static unsigned highest(void)
{
    if (occupied == 0) {
        return SCHED_PRIORITIES;
    }

    return 31u - (unsigned)__builtin_clz(occupied);
}

static void prio_enqueue(struct thread *t)
{
    unsigned level = level_of(t);

    t->sched.next = NULL;
    occupied |= 1u << level;

    if (tail[level] == NULL) {
        head[level] = t;
        tail[level] = t;
        return;
    }

    tail[level]->sched.next = t;
    tail[level] = t;
}

static struct thread *prio_pick_next(void)
{
    unsigned level = highest();
    struct thread *t;

    if (level >= SCHED_PRIORITIES) {
        return NULL;
    }

    t = head[level];

    head[level] = t->sched.next;

    if (head[level] == NULL) {
        tail[level] = NULL;
        occupied &= ~(1u << level);
    }

    t->sched.next = NULL;

    /* A fresh turn starts here rather than at enqueue, so a thread that
     * yields repeatedly without ever being chosen does not accumulate
     * one. */
    t->sched.quantum = quantum_ticks;

    return t;
}

static bool prio_ready(void)
{
    return occupied != 0;
}

static bool prio_tick(struct thread *running)
{
    if (running->sched.quantum > 0) {
        running->sched.quantum--;
        return false;
    }

    return true;
}

/*
 * Should `woken` take the CPU from `running` now, rather than when its
 * quantum runs out?
 *
 * Strictly greater, not greater-or-equal: a peer waits its turn, which is
 * what keeps round robin round. Only a superior interrupts.
 */
static bool prio_preempts(const struct thread *running,
                          const struct thread *woken)
{
    return level_of(woken) > level_of(running);
}

void sched_set_quantum(unsigned ticks)
{
    quantum_ticks = ticks == 0 ? 1 : ticks;
}

unsigned sched_get_quantum(void)
{
    return quantum_ticks;
}

const struct scheduler sched_priority = {
    .name      = "priority",
    .init      = prio_init,
    .enqueue   = prio_enqueue,
    .pick_next = prio_pick_next,
    .ready     = prio_ready,
    .tick      = prio_tick,
    .preempts  = prio_preempts,
};
