/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The two APIC registers, asked what real machines hold in them.
 *
 * **Every value below is one a register actually held.** The ThinkPad T14
 * Gen 2's I/O APIC is `version 32, address 0xfec00000, GSI 0-119` in a Linux
 * boot log from that machine - 0x00770020, a hundred and twenty inputs - and
 * no QEMU configuration produces it, because QEMU's I/O APIC has twenty-four
 * whatever the machine. That is the case `apic.c` refused for months as "an
 * impossible size", so it is the case this exists to keep answered.
 *
 * Same split as `tools/test_pmmplace.c` and for the same reason: the decision
 * is arithmetic, so it is asked on the host where every awkward case is one
 * line.
 */

#include <stdio.h>

#include "../hal/pc/apic_decode.h"

static int checks;
static int fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  %s\n", what);
    }
}

int main(void)
{
    /* 1. The machines this has met. */
    check(ioapic_inputs(0x00770020u) == 120,
          "the T14 Gen 2's I/O APIC (GSI 0-119) is not 120 inputs");
    check(ioapic_inputs(0x00170020u) == 24,
          "QEMU's I/O APIC is not 24 inputs");
    check(ioapic_inputs(0x00170011u) == 24,
          "the 82093AA's I/O APIC is not 24 inputs");

    /* 2. The edges of an eight-bit field, both of which are real chips. */
    check(ioapic_inputs(0x00000020u) == 1,
          "a Maximum Redirection Entry of 0 is not one input");
    check(ioapic_inputs(0x00ff0020u) == 256,
          "a Maximum Redirection Entry of 255 is not 256 inputs");

    /* 3. Nothing there. An unmapped or undecoded address reads all ones. */
    check(ioapic_inputs(0xffffffffu) == 0,
          "a register that reads all ones was taken for a chip");

    /* 4. The local APIC's mode, from IA32_APIC_BASE. 0xFEE00000 is where
     *    firmware puts it; bit 8 marks the bootstrap processor. */
    check(lapic_mode_of(0xfee00900ULL) == LAPIC_XAPIC,
          "enabled, bootstrap processor, no EXTD was not the MMIO mode");
    check(lapic_mode_of(0xfee00800ULL) == LAPIC_XAPIC,
          "enabled on an application processor was not the MMIO mode");
    check(lapic_mode_of(0xfee00d00ULL) == LAPIC_X2APIC,
          "enabled with EXTD set was not x2APIC mode");
    check(lapic_mode_of(0xfee00100ULL) == LAPIC_DISABLED,
          "the enable bit clear was not disabled");
    check(lapic_mode_of(0xfee00500ULL) == LAPIC_INVALID,
          "EXTD without the enable bit was not called invalid");

    /* 5. The address bits must not move the answer, since a firmware may
     *    relocate the APIC anywhere. */
    check(lapic_mode_of(0x0000000123400c00ULL) == LAPIC_X2APIC,
          "a relocated base changed what the mode bits say");

    if (fails > 0) {
        printf("FAIL: %d of %d checks on what the APIC registers say.\n",
               fails, fails + checks);
        return 1;
    }

    printf("PASS: %d checks on what the APIC registers say, on this machine "
           "(a hundred and twenty I/O APIC inputs, which no QEMU "
           "configuration has).\n", checks);
    return 0;
}
