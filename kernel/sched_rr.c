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
    return t;
}

static bool rr_tick(struct thread *running)
{
    /*
     * Round robin without a quantum: a thread runs until it yields or
     * blocks. Returning true here would ask for a preemption that nothing
     * can perform yet, and a policy that asks for something impossible is
     * worse than one that does not ask.
     *
     * The quantum belongs here, in `sched.quantum`, on the day the vector
     * epilogue can act on the answer.
     */
    (void)running;
    return false;
}

const struct scheduler sched_round_robin = {
    .name      = "round-robin",
    .init      = rr_init,
    .enqueue   = rr_enqueue,
    .pick_next = rr_pick_next,
    .tick      = rr_tick,
};
