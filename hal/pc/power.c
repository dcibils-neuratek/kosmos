/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Turning the machine off, and starting it again.
 *
 * `hal/qemu-virt/power.c` calls PSCI, which is one instruction and a
 * function number: ARM specified this and every implementation agrees. The
 * PC has no such thing. Powering off is ACPI, which means finding and
 * parsing tables to learn which port to write and what value - real work,
 * for one write.
 *
 * So this took the shortcut QEMU offers: on the q35 machine the ACPI PM1a
 * control block is at 0x604, and writing SLP_TYP=0 with SLP_EN set enters
 * S5. On the ThinkPad the same write is sleep type 0 - S0, where it already
 * was - and the machine halted with its last frame on the screen, which
 * nobody noticed until the power button became a key that shuts down.
 *
 * **Now it reads both from the firmware** (`acpi_s5`): the control block
 * from the FADT, and the sleep type from the DSDT's `\_S5` - 0 on q35 and 7
 * on the T14 - found by `s5_decode.c` without interpreting any AML. The two
 * writes are the order ACPICA uses: the type first, then the type with
 * SLP_EN, each keeping the bits around it (SCI_EN among them). QEMU's
 * constant stays as what is written when the firmware said nothing.
 *
 * Restart is different and is genuinely general: pulse the 8042 keyboard
 * controller's reset line, which is how a PC has been restarted since 1984
 * and works whether or not there is a keyboard attached.
 */

#include "acpi.h"
#include "hal.h"
#include "pc.h"

/* q35's ACPI PM1a control block. See above: this is QEMU's, not a PC's. */
#define QEMU_ACPI_PM1A  0x604
#define SLP_EN          (1u << 13)
#define SLP_TYP_SHIFT   10
#define SLP_TYP_MASK    (7u << SLP_TYP_SHIFT)

#define PS2_COMMAND     0x64
#define PS2_STATUS      0x64
#define PS2_RESET       0xFE

static void out16(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" :: "a"(value), "Nd"(port));
}

void hal_power_off(void)
{
    unsigned control, type;

    if (acpi_s5(&control, &type)) {
        uint16_t value = pc_in16((uint16_t)control);

        value = (uint16_t)((value & ~(SLP_TYP_MASK | SLP_EN))
                           | (type << SLP_TYP_SHIFT));
        out16((uint16_t)control, value);
        out16((uint16_t)control, (uint16_t)(value | SLP_EN));
    } else {
        out16(QEMU_ACPI_PM1A, SLP_EN);
    }

    /* If that did nothing - a real machine, or ACPI disabled - stop rather
     * than return. A caller of this has already decided the machine is
     * finished, and running on after it would be worse than halting. */
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

void hal_restart(void)
{
    unsigned tries;

    /*
     * The input buffer has to be empty before a command is accepted, and
     * bit 1 of the status port says whether it is. Bounded rather than
     * spun on: a controller that never drains is a machine that hangs here
     * instead of restarting, and there is a bigger hammer below.
     */
    for (tries = 0; tries < 100000u; tries++) {
        if ((pc_in8(PS2_STATUS) & 0x02) == 0) {
            break;
        }
    }

    pc_out8(PS2_COMMAND, PS2_RESET);

    /*
     * And if the pulse did not arrive, a triple fault will.
     *
     * Loading a null IDT and then taking an interrupt means the processor
     * cannot find a handler, cannot find the double-fault handler either,
     * and resets - which is the last resort every x86 kernel keeps, and the
     * only one that needs no cooperation from any device.
     */
    {
        struct { uint16_t limit; uint64_t base; } __attribute__((packed))
            nothing = { 0, 0 };

        __asm__ volatile("lidt %0" :: "m"(nothing));
        __asm__ volatile("int3");
    }

    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
