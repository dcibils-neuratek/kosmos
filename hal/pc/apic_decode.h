/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What two APIC registers mean, as arithmetic with no hardware in it.
 *
 * Split out of `apic.c` for the reason `loader_fb.c` and `pmm_place.c` were:
 * the case that matters is one QEMU cannot produce. QEMU's I/O APIC has
 * twenty-four inputs whatever the machine; the chipset in a ThinkPad T14
 * Gen 2 has a hundred and twenty, and `apic.c` refused every I/O APIC with
 * more than sixty-four as "an impossible size". So the first real machine
 * never left the 8259 pair - and, because the refusal was kept rather than
 * printed, never said why. `tools/test_apicdecode.c` asks these the awkward
 * questions on the host.
 */
#ifndef KOSMOS_HAL_PC_APIC_DECODE_H
#define KOSMOS_HAL_PC_APIC_DECODE_H

#include <stdint.h>

/*
 * How many inputs an I/O APIC has, from its version register - or zero when
 * nothing answered.
 *
 * 82093AA datasheet, section 3.2: bits 23:16 are the Maximum Redirection
 * Entry, the count minus one, and bits 7:0 the version. An eight-bit field,
 * so every value from 1 to 256 is a chip that exists. A register that reads
 * all ones is an address nothing decodes, which is the one reading that
 * says so.
 */
unsigned ioapic_inputs(uint32_t version);

/*
 * Which mode the local APIC is in, from IA32_APIC_BASE (MSR 0x1B).
 *
 * Intel SDM volume 3, chapter 11: bit 11 enables the APIC and bit 10
 * selects x2APIC. **In x2APIC mode the memory-mapped registers are gone** -
 * reads return all ones and writes vanish - so a driver that speaks MMIO has
 * to know before it touches one. x2APIC without enable is a combination the
 * SDM calls invalid.
 */
enum lapic_mode {
    LAPIC_DISABLED,
    LAPIC_XAPIC,
    LAPIC_X2APIC,
    LAPIC_INVALID,
};

enum lapic_mode lapic_mode_of(uint64_t apic_base);

#endif
