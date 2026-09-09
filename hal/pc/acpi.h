/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What the firmware says this machine is.
 *
 * `docs/targets.md` puts it plainly: a PC enumerates itself. There is no
 * device tree and no `fw_cfg` on a real one - ACPI says what the platform
 * is and PCI says what is plugged into it. This is the first half.
 *
 * **No AML, and that is the line.** The tables this reads are fixed-layout
 * structures a C compiler can describe. `\_SB` and the rest of the
 * namespace is a bytecode language with its own interpreter, and nothing
 * here needs it: the processor count, the interrupt controllers and where
 * PCIe's configuration space lives are all in tables, not in methods.
 */
#ifndef KOSMOS_HAL_PC_ACPI_H
#define KOSMOS_HAL_PC_ACPI_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Finds the tables and reads the ones that matter. False when there are
 * none, which is not an error: a machine booted without ACPI is a machine
 * with one processor and no ECAM, and everything below says so.
 */
bool acpi_init(void);

/*
 * Processors with a local APIC that the firmware marked usable.
 *
 * Zero when there are no tables. **Counted rather than assumed**: the MADT
 * lists one entry per local APIC and a machine may list more than it has
 * enabled - a disabled entry is a socket that is empty or a core the
 * firmware turned off, and counting those would promise processors that
 * are not there.
 */
unsigned acpi_cpu_count(void);

/* Where the local APIC's registers are, or 0. */
uint64_t acpi_lapic_base(void);

/* The first I/O APIC's registers, or 0. */
uint64_t acpi_ioapic_base(void);

/*
 * PCIe configuration space, from MCFG, or 0 when the firmware did not say.
 *
 * The difference this makes is not speed. Port 0xCF8 reaches 256 bytes of
 * configuration space per function; ECAM reaches 4096, and everything PCIe
 * added - capabilities that say what a link can do, and where MSI-X tables
 * live - is above the first 256.
 */
uint64_t acpi_ecam_base(void);

#endif
