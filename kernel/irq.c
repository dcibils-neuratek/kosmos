/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Hardware interrupts, delivered to a process. `kernel/irq.h` is the
 * argument; this is the table and the three moments that touch it.
 *
 * **One lock, and it masks interrupts like every other lock here.** That is
 * not a choice: `irq_deliver` runs from the interrupt handler, so a lock
 * held with interrupts on would deadlock against a tick the moment anything
 * else took it. The critical sections are correspondingly short - a scan of
 * sixteen entries, a counter, and a wake.
 */

#include <limits.h>

#include "irq.h"

#include "cpu.h"
#include "hal.h"
#include "ipc.h"
#include "kernel.h"
#include "process.h"
#include "spinlock.h"
#include "syscall.h"
#include "thread.h"

static struct irq_line lines[IRQ_LINES_MAX];
static struct spinlock lines_lock = SPINLOCK("irq");

void irq_init(void)
{
    size_t i;

    for (i = 0; i < IRQ_LINES_MAX; i++) {
        lines[i].in_use = false;
        lines[i].intid = 0;
        lines[i].pending = 0;
        lines[i].waiter = NULL;
        lines[i].owner = NULL;
        /* `generation` is deliberately not reset: it only ever counts up, so
         * that a capability naming a slot from before cannot match. */
    }
}

/*
 * Claim a line for a process.
 *
 * Refused when the controller says the number is not one a driver may have -
 * the timer and the inter-processor interrupt are the kernel's, and a
 * process that could claim the timer could stop the machine scheduling.
 * That decision belongs to the board, which is the only thing that knows
 * which numbers it uses, so `hal_irq_available` is asked rather than a list
 * being kept here.
 *
 * Refused when somebody already holds it, because two drivers on one line
 * would each be told about the other's interrupts and neither could quieten
 * the device.
 */
struct irq_line *irq_claim(unsigned intid, struct process *owner)
{
    unsigned long flags;
    struct irq_line *got = NULL;
    size_t i;

    if (!hal_irq_available(intid)) {
        return NULL;
    }

    flags = spin_lock(&lines_lock);

    for (i = 0; i < IRQ_LINES_MAX; i++) {
        if (lines[i].in_use && lines[i].intid == intid) {
            spin_unlock(&lines_lock, flags);
            return NULL;                    /* already somebody's */
        }
    }

    for (i = 0; i < IRQ_LINES_MAX; i++) {
        if (!lines[i].in_use) {
            got = &lines[i];
            break;
        }
    }

    if (got != NULL) {
        got->in_use = true;
        got->intid = intid;
        got->pending = 0;
        got->waiter = NULL;
        got->owner = owner;
        got->generation++;
    }

    spin_unlock(&lines_lock, flags);

    /*
     * Unmasked only once the slot exists, so an interrupt that arrives
     * between the two finds a claim rather than nobody. The other order
     * would deliver into a table that does not know about the line yet,
     * which on a level-triggered source is the livelock this whole design
     * exists to avoid.
     */
    if (got != NULL) {
        hal_irq_set_masked(intid, false);
    }

    return got;
}

void irq_release(struct irq_line *line)
{
    unsigned long flags;
    struct thread *waiter = NULL;
    unsigned intid;

    if (line == NULL) {
        return;
    }

    flags = spin_lock(&lines_lock);

    if (!line->in_use) {
        spin_unlock(&lines_lock, flags);
        return;
    }

    intid = line->intid;
    waiter = line->waiter;

    line->in_use = false;
    line->waiter = NULL;
    line->owner = NULL;
    line->pending = 0;
    line->generation++;

    spin_unlock(&lines_lock, flags);

    /*
     * Masked on the way out, and this is the one that matters when a driver
     * dies rather than exits. Its device is still asserting; nothing is left
     * to service it; an unmasked level line would deliver for ever into a
     * table entry nobody owns.
     */
    hal_irq_set_masked(intid, true);

    /*
     * And whoever was waiting is woken to find the line gone. A thread
     * blocked on a capability that was destroyed has to be released, or a
     * driver that is being killed never finishes dying - the same hazard
     * `ipc.c` handles when an endpoint's process ends.
     */
    if (waiter != NULL) {
        thread_wake(waiter);
    }
}

void irq_release_owned_by(struct process *owner)
{
    size_t i;

    if (owner == NULL) {
        return;
    }

    /*
     * Read under the lock and released outside it, one at a time, because
     * `irq_release` masks and wakes - neither of which may happen with the
     * table locked.
     */
    for (i = 0; i < IRQ_LINES_MAX; i++) {
        bool mine;
        unsigned long flags = spin_lock(&lines_lock);

        mine = lines[i].in_use && lines[i].owner == owner;

        spin_unlock(&lines_lock, flags);

        if (mine) {
            irq_release(&lines[i]);
        }
    }
}

/*
 * An interrupt arrived. **This runs in the handler, with interrupts masked.**
 *
 * Everything it does is bounded and none of it can block: a scan of sixteen
 * entries, a mask, a counter, and at most one wake.
 */
bool irq_deliver(unsigned intid)
{
    unsigned long flags;
    struct thread *waiter = NULL;
    bool mine = false;
    size_t i;

    flags = spin_lock(&lines_lock);

    for (i = 0; i < IRQ_LINES_MAX; i++) {
        if (!lines[i].in_use || lines[i].intid != intid) {
            continue;
        }

        mine = true;

        /*
         * Saturating, and the reason is in `irq.h`: what a driver does with
         * "some arrived while you were busy" does not depend on how many,
         * and a count that wrapped to zero would say none arrived at all.
         */
        if (lines[i].pending < UINT_MAX) {
            lines[i].pending++;
        }

        waiter = lines[i].waiter;
        lines[i].waiter = NULL;
        break;
    }

    spin_unlock(&lines_lock, flags);

    if (mine) {
        /*
         * Masked before the wake and before the handler returns. A
         * level-triggered source is still asserting: unmasked, the
         * controller would deliver it again immediately and the driver -
         * which is a process, and cannot run until this handler returns -
         * would never get a turn. `SYS_IRQ_ACK` is what unmasks it again.
         */
        hal_irq_set_masked(intid, true);

        if (waiter != NULL) {
            thread_wake(waiter);
        }
    }

    return mine;
}

/*
 * Wait for the next one.
 *
 * Takes a pending interrupt if there is one, and blocks otherwise. The two
 * have to be decided under the same lock the handler takes, or an interrupt
 * arriving between the test and the block would find no waiter, set no
 * wake, and leave the driver asleep with work outstanding - which is the
 * classic lost-wakeup and is exactly what `thread_block_and_release` exists
 * to prevent: it blocks and drops the lock at the one instant where this
 * thread is both findable and asleep.
 */
long irq_wait(struct irq_line *line, unsigned long ticks)
{
    struct thread *self = thread_current();
    uint64_t deadline = 0;
    unsigned long flags;

    if (line == NULL) {
        return SYS_ERR_DENIED;
    }

    /*
     * **A deadline, when one is asked for**, in the counter's clock like
     * every deadline in the kernel (`thread.h` says why). Taken once, before
     * the loop, so a wake that turns out to be for nothing does not start the
     * wait again from the beginning.
     *
     * It exists because a driver on real hardware cannot know its device
     * will ever interrupt. Without it, an xHCI controller whose MSI never
     * arrives is a driver asleep for good and a machine that says nothing -
     * and the ThinkPad is where that would happen first.
     */
    if (ticks != 0) {
        deadline = thread_deadline_in(ticks);
    }

    for (;;) {
        flags = spin_lock(&lines_lock);

        if (!line->in_use) {
            spin_unlock(&lines_lock, flags);
            return SYS_ERR_DENIED;          /* released underneath us */
        }

        if (line->pending > 0) {
            line->pending--;
            spin_unlock(&lines_lock, flags);
            return 0;
        }

        /*
         * The deadline, looked at under the lock and after `pending`, so an
         * interrupt that arrived as the deadline passed is taken rather than
         * reported missing.
         *
         * **And this thread takes itself off the line.** `irq_deliver` and
         * `irq_release` clear `waiter` before they wake it; a deadline clears
         * nothing. Left there, the next interrupt would wake a thread that
         * has already returned and gone on to wait for something else - the
         * hazard `ipc_receive`'s timeout takes itself off its queue for.
         */
        if (deadline != 0 && cpu_cycles() >= deadline) {
            if (line->waiter == self) {
                line->waiter = NULL;
            }

            spin_unlock(&lines_lock, flags);
            return SYS_NO_INTERRUPT;
        }

        /*
         * One waiter, and a second is refused rather than queued. A line
         * belongs to one driver and a driver waits on it from one thread;
         * two threads waiting would be a design nobody has asked for, and
         * guessing at which should be woken is the kind of policy that is
         * wrong in a way nothing catches.
         */
        if (line->waiter != NULL && line->waiter != self) {
            spin_unlock(&lines_lock, flags);
            return SYS_ERR_DENIED;
        }

        line->waiter = self;
        self->wake_at = deadline;

        thread_block_and_release(&lines_lock, flags);

        self->wake_at = 0;

        /* Woken: an interrupt, the line going away, or the deadline. Round
         * again, and the loop decides which. */
    }
}

/*
 * **Several lines, one thread, and whichever interrupts first.**
 *
 * `irq_wait` takes one line, and a driver of one device needs no more. The
 * xHCI driver is one process driving every USB controller on the machine -
 * two on the ThinkPad - from one thread, and it waited on each in turn for a
 * twentieth of a second. A plug or an unplug can afford that. A mouse cannot:
 * its reports come every few milliseconds, and one on the second controller
 * would wait out the first controller's nap and reach the pointer in bursts,
 * ten a second.
 *
 * So the wait is on all of them, and everything `irq_wait` promises holds by
 * the same means: every line is looked at, and this thread recorded as each
 * one's waiter, under `lines_lock`, and it blocks and lets the lock go in one
 * step. A delivery on any of the lines either comes first and is seen, or
 * comes after and finds a waiter to wake.
 *
 * **And it takes itself off every line before it returns.** A delivery clears
 * the waiter of its own line and no other, and a deadline clears none, so a
 * line left naming this thread would wake it later out of whatever it was
 * waiting for by then - the hazard `irq_wait`'s deadline handles for its one
 * line, here for the rest.
 *
 * One interrupt is taken, from the lowest line that has one. A driver that
 * looks at every device it holds after any wake loses nothing by that, and a
 * line still pending makes the next wait return at once.
 *
 * **And an endpoint, when `endpoint` is not negative** (USB step 5c,
 * `usb.md` §7). A driver with clients waits for them on the same wait, rather
 * than looking at its endpoint between interrupts - which on an idle machine
 * would make every request wait out a nap. A caller already queued answers
 * `IRQ_WAIT_CALLER`, for a receive that does not block to collect; otherwise
 * this thread becomes the endpoint's watcher, which `ipc_call` wakes. **A line
 * with an interrupt is answered before a caller**, so a stream of requests
 * cannot hold off a mouse.
 *
 * **Two locks, and no wake lost between them.** Every round takes the
 * endpoint's lock first and the lines' second, looks and records with both
 * held, then lets the endpoint's go - into the masked state, because the
 * lines' is still held - and blocks releasing the lines' into the state from
 * before either. A caller wakes a watcher only under the lines' lock
 * (`irq_wake_watcher`), so it cannot land between the endpoint's release and
 * the block, where `thread_wake` - which does nothing to a thread that is not
 * blocked - would lose it. An interrupt needs nothing new: it takes the lines'
 * lock as it always has. Nothing takes the two in the other order.
 */
long irq_wait_any(struct irq_line *const *set, unsigned count,
                  unsigned long ticks, int endpoint)
{
    struct thread *self = thread_current();
    uint64_t deadline = 0;
    unsigned i;

    if (set == NULL || count == 0) {
        return SYS_ERR_DENIED;
    }

    for (i = 0; i < count; i++) {
        if (set[i] == NULL) {
            return SYS_ERR_DENIED;
        }
    }

    if (ticks != 0) {
        deadline = thread_deadline_in(ticks);
    }

    for (;;) {
        struct endpoint *ep = NULL;
        unsigned long epflags = 0, flags;
        long answer = SYS_NO_INTERRUPT;
        bool done = false;

        if (endpoint >= 0) {
            ep = ipc_endpoint_lock(self, endpoint, &epflags);
        }

        flags = spin_lock(&lines_lock);

        /* An endpoint that names nothing, or went away while this slept. */
        if (endpoint >= 0 && ep == NULL) {
            answer = SYS_ERR_DENIED;
            done = true;
        }

        /* A line released underneath, or somebody else's wait: refused, as
         * `irq_wait` refuses both. */
        for (i = 0; i < count && !done; i++) {
            if (!set[i]->in_use
                || (set[i]->waiter != NULL && set[i]->waiter != self)) {
                answer = SYS_ERR_DENIED;
                done = true;
            }
        }

        for (i = 0; i < count && !done; i++) {
            if (set[i]->pending > 0) {
                set[i]->pending--;
                answer = (long)i;
                done = true;
            }
        }

        if (!done && ep != NULL && ipc_endpoint_has_caller(ep)) {
            answer = IRQ_WAIT_CALLER;
            done = true;
        }

        /* After the counts and the caller, so an interrupt or a call that
         * came as the deadline passed is taken rather than reported missing. */
        if (!done && deadline != 0 && cpu_cycles() >= deadline) {
            done = true;
        }

        /* One watcher, as `ipc_wait_for_caller` allows one. */
        if (!done && ep != NULL && !ipc_endpoint_watch(ep, self)) {
            answer = SYS_ERR_DENIED;
            done = true;
        }

        if (done) {
            for (i = 0; i < count; i++) {
                if (set[i]->waiter == self) {
                    set[i]->waiter = NULL;
                }
            }

            spin_unlock(&lines_lock, flags);

            if (ep != NULL) {
                ipc_endpoint_unwatch(ep, self);
                ipc_endpoint_unlock(ep, epflags);
            } else if (endpoint >= 0) {
                self->ipc.watching = NULL;      /* its endpoint is gone */
            }

            return answer;
        }

        for (i = 0; i < count; i++) {
            set[i]->waiter = self;
        }

        self->wake_at = deadline;

        if (ep != NULL) {
            ipc_endpoint_unlock(ep, flags);     /* masked: lines' still held */
            flags = epflags;
        }

        thread_block_and_release(&lines_lock, flags);

        self->wake_at = 0;

        /* Woken: an interrupt on one of them, a caller, a line or the
         * endpoint going away, or the deadline. Round again, and the loop
         * decides which. */
    }
}

/*
 * **A watcher woken under the lines' lock**, for `ipc.c`, which calls this
 * holding an endpoint's lock. `irq_wait_any` with an endpoint lets that
 * endpoint's lock go before it blocks and holds this one until it has, so a
 * wake taken under this lock finds it blocked; a bare `thread_wake` could find
 * it between the two and do nothing. Always an endpoint's lock, then this one.
 */
void irq_wake_watcher(struct thread *t)
{
    unsigned long flags = spin_lock(&lines_lock);

    thread_wake(t);
    spin_unlock(&lines_lock, flags);
}

long irq_ack(struct irq_line *line)
{
    unsigned long flags;
    unsigned intid;

    if (line == NULL) {
        return SYS_ERR_DENIED;
    }

    flags = spin_lock(&lines_lock);

    if (!line->in_use) {
        spin_unlock(&lines_lock, flags);
        return SYS_ERR_DENIED;
    }

    intid = line->intid;

    spin_unlock(&lines_lock, flags);

    /*
     * Unmasked outside the lock, because the controller is MMIO and there is
     * no reason to hold a lock across it. On a source that is not a line -
     * an MSI - this does nothing, and `irq.h` says why that is correct
     * rather than missing.
     */
    hal_irq_set_masked(intid, false);

    return 0;
}
