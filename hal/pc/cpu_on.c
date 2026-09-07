/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Starting another processor, and on this board nothing can yet.
 *
 * AArch64 asks PSCI, which is firmware: one call, an address, and the core
 * is running. **x86 has no firmware to ask.** A processor is started by
 * sending it INIT and then two STARTUP inter-processor interrupts through
 * the *local APIC*, at a vector that names a page below 1 MB where a
 * sixteen-bit trampoline waits to climb back up through protected mode into
 * long mode - the same climb `boot/x86_64/start.S` already does once.
 *
 * None of which exists: this board drives the 8259 PIC and has no local
 * APIC driver, and the count of processors is in the ACPI MADT which
 * nothing parses. `docs/smp.md` calls x86 "the expensive half" for exactly
 * this, and `docs/targets.md` sizes the APIC work at about six hundred
 * lines - work a real PC needs anyway, because the 8254 is one timer for a
 * whole machine and SMP needs one per core.
 *
 * So this refuses, and refusing is not a failure. **In practice it is not
 * even asked**: `smp_start_others` stops earlier still, because
 * `cpu_secondary_entry` in `arch/x86_64/cpu.h` answers 0 - there is nowhere
 * to land a core, which is a separate missing thing from there being no way
 * to start one. Two refusals, kept apart on purpose, so that building the
 * APIC does not silently look like building the trampoline as well.
 *
 * Either way the machine runs on one processor exactly as it did.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hal.h"

bool hal_cpu_on(unsigned cpu, uintptr_t entry, unsigned long context)
{
    (void)cpu;
    (void)entry;
    (void)context;

    return false;
}
