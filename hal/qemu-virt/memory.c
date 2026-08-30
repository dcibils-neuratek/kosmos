/*
 * The memory map of QEMU's virt machine.
 *
 * Source: QEMU hw/arm/virt.c, `base_memmap`. RAM starts at 0x40000000 and
 * everything below it is devices: flash at 0, the GIC at 0x08000000, the
 * PL011 at 0x09000000.
 */

#include "hal.h"

#define RAM_BASE    0x40000000UL

/*
 * The size is not discovered, it is assumed, and it has to match the `-m` on
 * the QEMU line in the Makefile and in tools/run_tests.py. Claiming more RAM
 * than QEMU was given means handing out pages that are not there, and the
 * first write to one is a fault a long way from the cause.
 *
 * The honest answer is the device tree: QEMU leaves a DTB at the base of RAM
 * with the real size in it. Parsing it is a few hundred lines and it is also
 * how the Pi's memory map gets discovered rather than guessed, so it arrives
 * with the second target at M2 and this constant goes away then.
 */
#define RAM_SIZE    (512UL * 1024 * 1024)

void hal_ram_range(struct memrange *out)
{
    out->base = RAM_BASE;
    out->size = RAM_SIZE;
}
