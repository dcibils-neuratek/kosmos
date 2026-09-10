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

#include <stdbool.h>

#include "acpi.h"
#include "hal.h"

/*
 * **This returned 1 with a comment explaining why, and the comment was
 * right.** There was no way to know: the count is in the ACPI MADT, one
 * entry per local APIC, and nothing parsed it - so one was the honest
 * answer rather than a guess, because the kernel really was scheduling on
 * one processor.
 *
 * `hal/pc/acpi.c` is the asking. What comes back is what the firmware
 * listed and marked usable, which on the ThinkPad this is aimed at is eight
 * or twelve against the four this kernel has ever seen.
 *
 * **Counting them is not starting them.** `smp_start_others` asks
 * `hal_cpu_on` for each processor counted here, and `cpu_on.c` gives every
 * one a line in the boot log - refused and why, stopped and where, or
 * arrived - so a count the machine cannot act on reads as a count with the
 * reason beside it, rather than as processors it claims.
 *
 * Falls back to one, and it is the same one for the same reason: a machine
 * whose firmware has no tables has a processor this kernel is running on,
 * whatever it cannot enumerate.
 */
unsigned hal_cpu_count(void)
{
    static bool asked;
    unsigned n;

    if (!asked) {
        asked = true;
        (void)acpi_init();
    }

    n = acpi_cpu_count();

    return (n > 0) ? n : 1u;
}
