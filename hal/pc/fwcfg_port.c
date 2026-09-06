/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Where fw_cfg's DMA register is on a PC, and nothing else.
 *
 * The protocol is in `hal/fwcfg/fwcfg.c` and is the same device on both
 * machines; this is the difference. On `virt` the registers are MMIO out of
 * the device tree. Here they are I/O ports, because a PC has an I/O address
 * space and QEMU put them in it: 0x510 selects an item, 0x511 reads its
 * bytes, and 0x514 is the eight-byte DMA register this code uses.
 *
 * **The DMA register is big-endian and the port is not**, which is the one
 * thing worth being careful about. `outl` writes a little-endian word, and
 * the register wants the most significant half at the lower address with
 * each half itself byte-swapped - the same dance the MMIO board does, for
 * the same reason, through a different instruction.
 */

#include <stdint.h>

#include "fwcfg.h"
#include "pc.h"

/* QEMU's `hw/i386/fw_cfg.c`: FW_CFG_IO_BASE 0x510, and the DMA register
 * four bytes past the selector and data pair. */
#define FWCFG_PORT_DMA  0x514

static void out32(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" :: "a"(value), "Nd"(port));
}

static uint32_t in32(uint16_t port)
{
    uint32_t v;

    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));

    return v;
}

/*
 * Most significant half first, and **the write to the low half is what
 * starts the operation** - so the order is the protocol rather than a
 * preference.
 */
void fwcfg_reg_write(uint64_t value)
{
    out32(FWCFG_PORT_DMA + 0, __builtin_bswap32((uint32_t)(value >> 32)));
    out32(FWCFG_PORT_DMA + 4, __builtin_bswap32((uint32_t)value));
}

uint64_t fwcfg_reg_read(void)
{
    uint32_t high = __builtin_bswap32(in32(FWCFG_PORT_DMA + 0));
    uint32_t low  = __builtin_bswap32(in32(FWCFG_PORT_DMA + 4));

    return ((uint64_t)high << 32) | low;
}
