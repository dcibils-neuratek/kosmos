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

#include "sched.h"
#include "thread.h"
#include "panic.h"

/*
 * How many ticks a thread gets before it is taken off the CPU.
 *
 * Ten, so 100 ms at the 100 Hz the timer runs at. Long enough that a thread
 * doing ordinary work is never interrupted for no reason, short enough that
 * one that never yields cannot hold the machine.
 *
 * It is a property of this policy and nothing else knows it. A policy with a
 * different idea of fairness is a different file.
 */
#define RR_QUANTUM  10

static struct thread *head;
static struct thread *tail;

static void rr_init(void)
{
    head = NULL;
    tail = NULL;
}

static void rr_enqueue(struct thread *t)
{
    t->sched.next = NULL;

    if (tail == NULL) {
        head = t;
        tail = t;
        return;
    }

    tail->sched.next = t;
    tail = t;
}

static struct thread *rr_pick_next(void)
{
    struct thread *t = head;

    if (t == NULL) {
        return NULL;
    }

    head = t->sched.next;
    if (head == NULL) {
        tail = NULL;
    }

    t->sched.next = NULL;

    /* A fresh turn starts here rather than at enqueue, so a thread that
     * yields repeatedly without ever being chosen does not accumulate one. */
    t->sched.quantum = RR_QUANTUM;

    return t;
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
    .tick      = rr_tick,
};
