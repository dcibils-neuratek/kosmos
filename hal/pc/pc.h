/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef HAL_PC_H
#define HAL_PC_H

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

/* Where IRQ 0 lands once the 8259s have been remapped, clear of the 32
 * vectors the architecture reserves for exceptions. `pic.c` says why. */
#define PC_IRQ_BASE     32

/* Lets one interrupt line through. Everything starts masked. */
void pc_irq_unmask(unsigned irq);

/* What `pic.c` calls when IRQ 0 arrives. In `timer.c`, which owns the count. */
void pc_timer_interrupt(void);

#endif /* HAL_PC_H */
