/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Round robin.
 *
 * A FIFO queue: the thread that has waited longest runs next, and a thread
 * that yields goes to the back. Every thread gets a turn and none can be
 * starved by another, which is the only property that matters while there is
 * nothing to prioritise.
 *
 * A singly linked queue with a tail pointer, threaded through the thread's
 * own `sched.next`. No allocation, and enqueue and pick are both constant
 * time.
 *
 * This is the policy the IPC benchmark at the end of M3 is measured against,
 * so it is deliberately the simplest thing that is fair: any number it
 * produces is a property of the IPC path rather than of clever scheduling.
 */

#include <stddef.h>

#include "kernel.h"
#include "percpu.h"
#include "sched.h"
#include "thread.h"
#include "panic.h"

/*
 * How many ticks a thread gets before it is taken off the CPU.
 *
 * A tenth of a second: long enough that a thread doing ordinary work is
 * never interrupted for no reason, short enough that one that never yields
 * cannot hold the machine.
 *
 * From `TICK_HZ` rather than a count, so that changing the tick rate - which
 * happened, for the sound device - does not quietly change what fairness
 * means here as well.
 *
 * It is a property of this policy and nothing else knows it. A policy with a
 * different idea of fairness is a different file.
 */
#define RR_QUANTUM  (TICK_HZ / 10)

/*
 * One queue per processor, for the reason `sched_prio.c` gives at length: a
 * thread has a home core and does not migrate, so two cores scheduling
 * contend for nothing. The caller holds the queue's lock.
 */
static struct thread *head[NR_CPUS];
static struct thread *tail[NR_CPUS];

static void rr_init(void)
{
    unsigned c;

    for (c = 0; c < NR_CPUS; c++) {
        head[c] = NULL;
        tail[c] = NULL;
    }
}

static void rr_enqueue(unsigned cpu, struct thread *t)
{
    t->sched.next = NULL;

    if (tail[cpu] == NULL) {
        head[cpu] = t;
        tail[cpu] = t;
        return;
    }

    tail[cpu]->sched.next = t;
    tail[cpu] = t;
}

static struct thread *rr_pick_next(unsigned cpu)
{
    struct thread *t = head[cpu];

    if (t == NULL) {
        return NULL;
    }

    head[cpu] = t->sched.next;
    if (head[cpu] == NULL) {
        tail[cpu] = NULL;
    }

    t->sched.next = NULL;

    /* A fresh turn starts here rather than at enqueue, so a thread that
     * yields repeatedly without ever being chosen does not accumulate one. */
    t->sched.quantum = RR_QUANTUM;

    return t;
}

static bool rr_ready(unsigned cpu)
{
    return head[cpu] != NULL;
}

static bool rr_tick(struct thread *running)
{
    if (running->sched.quantum > 0) {
        running->sched.quantum--;
        return false;
    }

    /*
     * Its turn is over. Whether anything actually replaces it is not this
     * function's business: the policy says the thread has had enough, and
     * the mechanism decides there is somebody else to run.
     */
    return true;
}

const struct scheduler sched_round_robin = {
    .name      = "round-robin",
    .init      = rr_init,
    .enqueue   = rr_enqueue,
    .pick_next = rr_pick_next,
    .ready     = rr_ready,
    .tick      = rr_tick,
};
