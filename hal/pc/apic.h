/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The interrupt controller a machine built this decade actually has.
 *
 * **The 8259 pair this replaces is from 1981 and may not be there.** Intel
 * has been removing the legacy PIC and PIT from UEFI-only platforms, and a
 * machine without them driving `hal/pc/pic.c` gets no scheduler tick at
 * all - which presents as a boot that prints every stage and then stops,
 * with nothing else wrong. `docs/targets.md` §1 is the rule this follows:
 * drivers group by what the device is, and which one a machine wants is a
 * question its firmware answers at run time.
 *
 * So both exist and neither is chosen at build time. `irq_bind.c` asks ACPI
 * whether this machine reported an I/O APIC and takes that path when it
 * did, and `hal_irq_describe` says which in the boot log - because on a
 * machine with no serial port a wrong guess here is a silent hang.
 *
 *--------------------------------------------------------------------------
 * Two chips, and they do different jobs.
 *
 * **The local APIC is inside each processor.** It receives what is aimed at
 * that core, acknowledges it, and holds a timer of its own - which is the
 * part that matters most here, because a scheduler needs a tick per core
 * and the 8253 is one timer for a whole machine. It is also how a processor
 * is started at all: INIT and two STARTUP inter-processor interrupts, which
 * is what `hal/pc/cpu_on.c` has been waiting for.
 *
 * **The I/O APIC is on the chipset and routes.** A table with an entry per
 * input - twenty-four on the 82093AA and in QEMU, a hundred and twenty on a
 * ThinkPad T14 Gen 2's chipset - each saying which vector to raise and which
 * core to raise it on. The 8259 pair between them had fifteen lines and could
 * only ever interrupt the bootstrap processor.
 *
 * AArch64's GIC is the same split under other names, and `qemu-virt/gic.c`
 * already implements it: the redistributor is per-core and the distributor
 * routes. This is that, for the other architecture.
 *
 * Intel SDM volume 3, chapter 11 for the local APIC; the 82093AA I/O APIC
 * datasheet for the other half.
 */
#ifndef KOSMOS_HAL_PC_APIC_H
#define KOSMOS_HAL_PC_APIC_H

#include <stdbool.h>
#include <stdint.h>

/*
 * True when this machine has one and it is now running. False is not an
 * error: the 8259 pair is what runs, and `apic_describe` says why - no I/O
 * APIC described, one that does not answer, or a local APIC in a mode this
 * driver does not speak.
 */
bool apic_init(void);

bool apic_present(void);

/* Lets one line through, by the ISA number everything above still uses.
 * The override table is what turns that into an input. */
void apic_unmask(unsigned irq);
void apic_mask(unsigned irq);

/*
 * Serves whatever arrived and says whether it was the tick.
 *
 * Which interrupt it was comes from the local APIC's in-service register
 * rather than from an argument, so that `arch/` keeps knowing nothing about
 * what a controller is - the same shape `hal_irq_handle` has always had.
 */
bool apic_handle(void);

/* The tick, from the local APIC's own timer rather than from the 8253. */
bool apic_timer_init(unsigned hz);

/* This processor's local APIC id, which an MSI has to be addressed to. */
unsigned apic_id(void);

const char *apic_describe(void);

/*
 * This core's half, on a processor started after the first: accept every
 * priority, switch the local APIC on, and record its id so that another
 * core can interrupt this one.
 */
void apic_init_here(void);

/* This core's tick, at the rate core zero calibrated against the 8253. */
void apic_timer_init_here(void);

/*
 * INIT, then two STARTUP IPIs carrying `vector`, to the processor whose local
 * APIC id is `id`. False when there is no local APIC to send them with, or
 * the id is one this interface cannot address.
 */
bool apic_start_processor(uint32_t id, unsigned vector);

/* Interrupts another core so it looks at its runqueue. */
void apic_wake(unsigned cpu);

#endif
