/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KERNEL_IRQ_H
#define KERNEL_IRQ_H

#include <stdbool.h>
#include <stddef.h>

struct thread;
struct process;

/*
 * **A hardware interrupt, delivered to a process.**
 *
 * The third of `docs/drivers.md`'s primitives and the only one with real
 * design in it, because it is the only one that has to happen *inside an
 * interrupt handler, with everything masked*. The other two are syscalls
 * made by a thread that is running and can be told to wait; this one starts
 * in a place where nothing may block, nothing may allocate, and the cost of
 * being slow is measured against every other interrupt on the machine.
 *
 * --------------------------------------------------------------------
 * What it is not: a message.
 *
 * The obvious design is "the interrupt sends a message to an endpoint", and
 * it is wrong here for the reason `CLAUDE.md` gives about the audio server:
 * a stream of events that recurs because the hardware says so does not
 * travel as a payload. An interrupt carries no data at all - the data is in
 * the device's registers, which the driver already has mapped - so a message
 * would be an empty envelope, built and copied and queued, on the one path
 * in the system that runs with interrupts off.
 *
 * So it is a **wake**, which is the smallest thing that can happen here: a
 * counter goes up and a thread that was waiting stops waiting. No allocation,
 * no copy, no queue, and the work done with interrupts masked is a lock, a
 * few comparisons and a `thread_wake`.
 *
 * --------------------------------------------------------------------
 * Why the line is masked, and why that is not an optimisation.
 *
 * A level-triggered interrupt is asserted for as long as the device has
 * something to say. Returning from the handler without either servicing the
 * device or masking the line means the controller sees it still asserted and
 * delivers it again immediately - and the driver, which is a *process*, has
 * not had a chance to run in between. The machine livelocks in the handler
 * and nothing else ever executes again.
 *
 * The kernel cannot service the device: it does not know what these
 * registers mean, which is the entire point of the driver being outside it.
 * So it masks the line, and the driver unmasks it with `SYS_IRQ_ACK` once it
 * has quietened the device. That is the same bargain Linux's threaded
 * handlers and seL4's IRQHandler make, and for the same reason.
 *
 * **MSI is the exception and is handled by doing nothing.** A message
 * signalled interrupt is not a line: the device writes to the local APIC,
 * there is no redirection entry, and nothing is asserted afterwards. There
 * is nothing to mask and nothing to unmask, so the HAL's mask is a no-op for
 * those numbers and the ack costs a syscall and changes nothing. Written
 * down because "ack does nothing" looks like a bug to whoever reads it next,
 * and on the ThinkPad - where xHCI will use MSI - it is the normal case.
 *
 * --------------------------------------------------------------------
 * A capability, not a number.
 *
 * `irq_claim` hands back a `struct irq_line *` which the syscall installs in
 * the caller's capability table, so a driver holds an index into its own
 * table and cannot name a line it was not given. That is the rule the whole
 * system runs on, and it is what makes it possible to later have a devices
 * server that claims a line and passes the capability to the driver it
 * decides should own it - the driver never needing device authority at all.
 */

/*
 * How many lines may be claimed at once.
 *
 * Statically declared, because the kernel has no allocator. Sixteen is far
 * above the number of devices this system will drive from userland for a
 * long time - xHCI, NVMe, a network adapter and a codec is four - and small
 * enough that the table is a linear scan nobody needs to think about.
 */
#define IRQ_LINES_MAX 16

struct irq_line {
    unsigned        intid;      /* the controller's own number */
    bool            in_use;
    unsigned        generation; /* against a slot freed and reused */

    /*
     * Interrupts that have arrived and not yet been taken.
     *
     * A count rather than a flag, because the driver may be doing something
     * else when one arrives and must not be told a second one never
     * happened. It saturates rather than wrapping: what a driver does with
     * "some arrived while you were busy" is the same whatever the number,
     * and a count that wrapped to zero would say "none".
     */
    unsigned        pending;

    struct thread  *waiter;     /* blocked in irq_wait, or NULL */
    struct process *owner;
};

void             irq_init(void);

struct irq_line *irq_claim(unsigned intid, struct process *owner);
void             irq_release(struct irq_line *line);
void             irq_release_owned_by(struct process *owner);

/*
 * From the interrupt handler. True when a process owns this line, which is
 * what tells the HAL that the interrupt was somebody's rather than nobody's.
 */
bool             irq_deliver(unsigned intid);

long             irq_wait(struct irq_line *line);
long             irq_ack(struct irq_line *line);

#endif /* KERNEL_IRQ_H */
