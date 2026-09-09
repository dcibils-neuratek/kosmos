/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef HAL_PC_H
#define HAL_PC_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Shared between this board's own files. Not part of the HAL interface: no
 * other board implements these and nothing outside hal/pc/ may call them.
 *
 * `hal/qemu-virt/qemu-virt.h` is the same idea for the ARM board, and the
 * rule it states is the one that matters - a board's files may talk to each
 * other, and nothing above them may join in.
 */

/*
 * Port I/O, which is the whole of how a PC talks to its old peripherals.
 *
 * This is the x86 answer to `mmio_read32` / `mmio_write32`, and the reason
 * it exists in the same shape is the same: a device access should be one
 * named thing that carries whatever the architecture requires, rather than
 * a `volatile` and a hope. What it does *not* need is the barriers their
 * ARM counterparts carry - `in` and `out` are serialising on x86 by
 * definition, which is one of the few places this architecture asks for
 * less rather than more.
 *
 * The `"Nd"` constraint is "an 8-bit constant, or DX" - the two addressing
 * forms the instruction actually has.
 */
static inline void pc_out8(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint8_t pc_in8(uint16_t port)
{
    uint8_t v;

    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));

    return v;
}

/*
 * What a multiboot loader leaves behind, and where.
 *
 * Here rather than in the one file that reads it, because two of them do
 * now - `memory.c` wants the memory map and `boot.c` the command line -
 * and `hal/qemu-virt/qemu-virt.h` states the rule this follows: a layout
 * written down twice is a layout that can disagree with itself.
 *
 * `flags` says which of the fields below were filled in, one bit each.
 * Multiboot specification 0.6.96, section 3.3.
 */
#include "multiboot.h"


#endif /* HAL_PC_H */
