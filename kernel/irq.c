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

#include "hal.h"
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
long irq_wait(struct irq_line *line)
{
    unsigned long flags;

    if (line == NULL) {
        return SYS_ERR_DENIED;
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
         * One waiter, and a second is refused rather than queued. A line
         * belongs to one driver and a driver waits on it from one thread;
         * two threads waiting would be a design nobody has asked for, and
         * guessing at which should be woken is the kind of policy that is
         * wrong in a way nothing catches.
         */
        if (line->waiter != NULL) {
            spin_unlock(&lines_lock, flags);
            return SYS_ERR_DENIED;
        }

        line->waiter = thread_current();

        thread_block_and_release(&lines_lock, flags);

        /* Woken: either an interrupt arrived or the line went away. Round
         * again, and the loop decides which. */
    }
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
