/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * How many processors this machine has, and on this board the honest answer
 * is "one, because nothing here has asked".
 *
 * AArch64 gets this from PSCI, which will say whether a given processor
 * exists without starting it - see `hal/qemu-virt/power.c`. **x86 has no
 * equivalent.** The count lives in the ACPI MADT, one entry per local APIC,
 * and reaching it means finding the RSDP, walking the XSDT and parsing a
 * table - none of which exists here. `docs/targets.md` lists it under what
 * a real PC needs and sizes it at about four hundred lines.
 *
 * So this returns 1, which is *true* - the kernel is scheduling on one
 * processor - rather than 0, which would be false, or a guess, which would
 * be worse than either. The day ACPI is parsed this becomes a real count
 * and nothing above it changes.
 *
 * **And a count will not be the whole answer on this architecture.** Alder
 * Lake and everything after it are hybrid: performance cores and efficiency
 * cores, with different cache sizes and different peak clocks, and
 * `CPUID.1A` says which is which. `docs/smp.md` records why that changes
 * the shape of the interface rather than only its value.
 */

#include "hal.h"

unsigned hal_cpu_count(void)
{
    return 1;
}
