/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A processor's own interrupt controller and clock, and on this board there
 * is no such thing yet.
 *
 * AArch64 gives every core a GIC redistributor and a banked generic timer,
 * so a secondary arms its own comparator and receives its own PPI without
 * asking anybody. The PC equivalent is the **local APIC**: one per core, with
 * its own timer, and it is what `docs/smp.md` calls the expensive half. This
 * board drives the 8259 PIC and the 8254, which are one interrupt controller
 * and one timer for the whole machine - there is nothing here that belongs
 * to a processor rather than to the computer.
 *
 * So both of these do nothing, and doing nothing is correct rather than
 * unfinished: **they are only ever called on a secondary, and this board
 * cannot start one.** `arch/x86_64/cpu.h` has no trampoline to land a core
 * on and `hal/pc/cpu_on.c` has no APIC to send `INIT`-`SIPI`-`SIPI` with, so
 * `smp_start_others` stops before either. Core zero's own setup happens in
 * `hal_irq_init` and `hal_timer_init` exactly as it did.
 *
 * They exist because `kernel/smp.c` has no architecture in it and must link
 * on both boards - the same lesson `mmu_enable_here` taught when it was
 * called from there and x86-64 refused the link.
 */

#include "hal.h"

void hal_irq_init_here(void)
{
}

void hal_timer_init_here(void)
{
}
