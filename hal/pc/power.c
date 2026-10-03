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
 * **Restart is four ways, in an order** (`testing.md` 18.352). It was one -
 * pulse the 8042 keyboard controller's reset line, "how a PC has been
 * restarted since 1984", and a triple fault behind it - and on the M700 the
 * 8042 is the firmware playing one, which stops once the USB driver takes
 * the controller from it: Restart in the menu did nothing there (Diego, 3
 * October: "Right now only shutdown is done"). So, as Linux's
 * `native_machine_emergency_restart` would, first the way the firmware
 * says: the FADT's reset register, a byte written to an I/O port - 0xCF9
 * on an Intel chipset, and on QEMU's q35. Then that port by hand, as
 * Linux's `BOOT_CF9` does it: the hard-reset request, 50 microseconds, the
 * reset. Then the keyboard controller, and the triple fault last. Each says
 * so before it is tried, and the first that works is the last line.
 */

#include "acpi.h"
#include "console.h"
#include "cpu.h"
#include "hal.h"
#include "multiboot.h"
#include "pc.h"

/* q35's ACPI PM1a control block. See above: this is QEMU's, not a PC's. */
#define QEMU_ACPI_PM1A  0x604
#define SLP_EN          (1u << 13)
#define SLP_TYP_SHIFT   10
#define SLP_TYP_MASK    (7u << SLP_TYP_SHIFT)

#define PS2_COMMAND     0x64
#define PS2_STATUS      0x64
#define PS2_RESET       0xFE

/* The reset control register of Intel's chipsets (and QEMU's ICH9), as
 * Linux's `reboot.c` writes it: bit 1 asks for a hard reset, and 0x06 -
 * that and the reset bit - performs it. */
#define RESET_CONTROL   0xCF9
#define RESET_HARD      0x02
#define RESET_NOW       0x06

#define GAS_SYSTEM_IO   1u

/* At least `us` microseconds, on the counter: an x86 machine whose timer
 * has not measured it is assumed faster than any, so the wait is longer. */
static void pause_us(unsigned us)
{
    uint64_t hz = pc_timer_tsc_hz();
    uint64_t until;

    if (hz == 0) {
        hz = 8000000000ULL;
    }

    until = cpu_cycles() + hz / 1000000u * us;

    while (cpu_cycles() < until) {
    }
}

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
    unsigned tries, space;
    uint64_t address;
    uint8_t value, control;

    /* 1. The firmware's own way, when it names one in system I/O. */
    if (acpi_reset_register(&space, &address, &value)) {
        kputs("restart: the ACPI reset register, ");

        if (space == GAS_SYSTEM_IO && address <= 0xFFFFu) {
            kputs("port ");
            kputx(address, 4);
            kputs(" <- ");
            kputx(value, 2);
            kputs("\n");
            pc_out8((uint16_t)address, value);
            pause_us(50000);
        } else {
            kputs("in an address space this does not write; next\n");
        }
    }

    /* 2. The chipset's reset port, by hand, as Linux's BOOT_CF9 does. */
    kputs("restart: the reset port, 0xcf9\n");
    control = (uint8_t)(pc_in8(RESET_CONTROL) & ~RESET_NOW);
    pc_out8(RESET_CONTROL, (uint8_t)(control | RESET_HARD));
    pause_us(50);
    pc_out8(RESET_CONTROL, (uint8_t)(control | RESET_NOW));
    pause_us(50000);

    /*
     * 3. The keyboard controller. The input buffer has to be empty before a
     * command is accepted, and bit 1 of the status port says whether it is.
     * Bounded rather than spun on: a controller that never drains is a
     * machine that hangs here instead of restarting.
     */
    kputs("restart: the keyboard controller\n");

    for (tries = 0; tries < 100000u; tries++) {
        if ((pc_in8(PS2_STATUS) & 0x02) == 0) {
            break;
        }
    }

    pc_out8(PS2_COMMAND, PS2_RESET);
    pause_us(50000);

    /*
     * 4. And a triple fault, which needs no device at all.
     *
     * Loading a null IDT and then taking an interrupt means the processor
     * cannot find a handler, cannot find the double-fault handler either,
     * and resets - the last resort every x86 kernel keeps.
     */
    kputs("restart: a triple fault\n");

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
