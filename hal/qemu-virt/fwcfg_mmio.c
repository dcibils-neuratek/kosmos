/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Where fw_cfg's DMA register is on this board, and nothing else.
 *
 * The protocol is in `hal/fwcfg/fwcfg.c` and is the same on both machines;
 * this is the eight lines that differ. Read out of QEMU's device tree:
 * fw-cfg@9020000, reg = <0x0 0x9020000 0x0 0x18>. Twenty-four bytes, which
 * is exactly the three registers, and the DMA one is the last.
 */

#include <stdint.h>

#include "fwcfg.h"
#include "mmio.h"

#define FWCFG_BASE      0x09020000UL
#define FWCFG_REG_DMA   (FWCFG_BASE + 16)

/*
 * Two 32-bit halves, most significant first - and **the write to the low
 * half is what starts the operation**, so the order is the protocol rather
 * than a preference. Each half is itself big-endian, hence the swaps:
 * `mmio_write32` stores a little-endian word.
 */
void fwcfg_reg_write(uint64_t value)
{
    mmio_write32(FWCFG_REG_DMA + 0, __builtin_bswap32((uint32_t)(value >> 32)));
    mmio_write32(FWCFG_REG_DMA + 4, __builtin_bswap32((uint32_t)value));
}

uint64_t fwcfg_reg_read(void)
{
    uint32_t high = __builtin_bswap32(mmio_read32(FWCFG_REG_DMA + 0));
    uint32_t low  = __builtin_bswap32(mmio_read32(FWCFG_REG_DMA + 4));

    return ((uint64_t)high << 32) | low;
}
