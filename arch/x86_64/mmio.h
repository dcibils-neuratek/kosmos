/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef ARCH_X86_64_MMIO_H
#define ARCH_X86_64_MMIO_H

#include <stdint.h>

/*
 * The only way anything in Kosmos touches a memory-mapped device register.
 *
 * `arch/aarch64/mmio.h` is the counterpart and it is four times as long,
 * because almost everything it has to argue about is decided for it here.
 *
 * **No barriers, and that is the memory model rather than an omission.**
 * x86-64 is TSO: stores are not reordered with other stores, loads are not
 * reordered with other loads, and a store to a page mapped uncached - which
 * `MAP_DEVICE` makes it - is not buffered or combined. So the pairing that
 * file spends a page explaining, a barrier before a write and after a read,
 * is already what the processor does. The one reordering x86 does allow is
 * a later load passing an earlier store, and `mfence` is where that would
 * be answered if a driver ever needs it; nothing does yet, and putting one
 * here would slow every access to fix a case that has not arrived.
 *
 * **Written in assembly all the same, and for the ARM file's second
 * reason.** `*(volatile uint32_t *)addr = value` says what to store and
 * leaves *how* to the compiler, and `volatile` does not constrain that. On
 * ARM that produced a post-indexed store that no hypervisor can decode,
 * and Kosmos died under `-accel hvf` on the first MMIO write. x86's
 * trap-and-emulate path re-fetches and decodes the faulting instruction
 * rather than reading a syndrome register, so it can handle anything the
 * compiler emits - but a single instruction chosen here still costs nothing
 * and still means the access is the one that was written down.
 *
 * This does **not** cover port I/O, which is where most of a PC's older
 * devices live. That is `pc_out8` and `pc_in8` in `hal/pc/pc.h`, and it is
 * a different instruction with different rules: `in` and `out` are
 * serialising on their own and cannot be reordered against anything.
 */

static inline void mmio_write32(uintptr_t addr, uint32_t value)
{
    __asm__ volatile("movl %0, (%1)" :: "r"(value), "r"(addr) : "memory");
}

static inline uint32_t mmio_read32(uintptr_t addr)
{
    uint32_t value;

    __asm__ volatile("movl (%1), %0" : "=r"(value) : "r"(addr) : "memory");

    return value;
}

/*
 * The same, a byte at a time.
 *
 * The ARM file needs this for virtio's configuration space, whose first
 * three fields are single bytes. Nothing on this architecture has asked yet
 * - the PC's own devices answer through ports - but a device that decodes
 * the access width is entitled to, and reading a byte field as a word is
 * the kind of thing that works by accident until it does not.
 */

static inline void mmio_write8(uintptr_t addr, uint8_t value)
{
    __asm__ volatile("movb %0, (%1)" :: "r"(value), "r"(addr) : "memory");
}

static inline uint8_t mmio_read8(uintptr_t addr)
{
    uint8_t value;

    __asm__ volatile("movb (%1), %0" : "=r"(value) : "r"(addr) : "memory");

    return value;
}

#endif /* ARCH_X86_64_MMIO_H */
