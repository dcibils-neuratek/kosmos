/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A processor's own interrupt controller and clock, on a PC: its local APIC.
 *
 * AArch64 gives every core a GIC redistributor and a banked generic timer;
 * the PC equivalent is the local APIC, one per core with a timer of its own,
 * and `apic.c` drives it. These run on every processor started after the
 * first, from `kernel/smp.c`'s `secondary_main`. Core zero's own setup
 * happens in `hal_irq_init` and `hal_timer_init` exactly as it always did.
 *
 * On a machine running the 8259 pair nothing here is ever called, because
 * `cpu_on.c` refuses to start a processor without a local APIC to start it
 * with - and each of these is a no-op there anyway.
 */

#include "apic.h"
#include "hal.h"

void hal_irq_init_here(void)
{
    apic_init_here();
}

void hal_timer_init_here(void)
{
    apic_timer_init_here();
}

/*
 * Knock on another processor.
 *
 * The point is the interruption, not a message: `thread_wake` has already put
 * a thread on that core's runqueue, and the interrupt makes the core look.
 * Its handler does nothing, and the way out of it runs the scheduler.
 */
void hal_cpu_wake(unsigned cpu)
{
    apic_wake(cpu);
}
