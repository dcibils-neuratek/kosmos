#ifndef ARCH_AARCH64_MMIO_H
#define ARCH_AARCH64_MMIO_H

#include <stdint.h>

/*
 * The only way anything in Kosmos touches a device register.
 *
 * The barriers are the whole reason these exist rather than a `volatile`
 * pointer at each call site. AArch64 has a weak memory model, and the
 * compiler is free to reorder any non-volatile access across a volatile
 * one, so ordering between normal memory and a device has to be stated
 * rather than hoped for.
 *
 * The pairing is the one Linux uses, and it is the minimum that is correct:
 *
 *   write: the barrier goes BEFORE the store. Whatever the driver placed in
 *          normal memory (a descriptor, a buffer) has to be observable to the
 *          device before the register write that tells it to go look.
 *
 *   read:  the barrier goes AFTER the load. The value has to be in hand
 *          before any later access executes, or a polling loop gets
 *          reordered into acting on a status flag it has not read yet.
 *
 * `osh`, outer shareable: a device sits outside the inner shareable domain
 * the CPUs share, so `ish` would not order against it.
 *
 * `dmb` and not `dsb`: what is needed here is ordering, not completion. A
 * `dsb` additionally stalls until the access has finished, which costs more
 * and buys nothing at this layer. The places that genuinely need completion
 * — enabling the MMU, cache maintenance, waking a parked core — issue their
 * own `dsb` and say why at the call site.
 */

static inline void mmio_write32(uintptr_t addr, uint32_t value)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)addr = value;
}

static inline uint32_t mmio_read32(uintptr_t addr)
{
    uint32_t value = *(volatile uint32_t *)addr;
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}

/*
 * The same, a byte at a time.
 *
 * Only one thing needs this: a virtio device's configuration space, whose
 * first three fields are single bytes - a selector, a sub-selector and a
 * length - and which is how a driver asks a device what it is. Reading that
 * as a 32-bit word would work by accident on this machine and stop working
 * on one that decodes the access width, which devices are allowed to do.
 */

static inline void mmio_write8(uintptr_t addr, uint8_t value)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint8_t *)addr = value;
}

static inline uint8_t mmio_read8(uintptr_t addr)
{
    uint8_t value = *(volatile uint8_t *)addr;
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}

#endif /* ARCH_AARCH64_MMIO_H */
