/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef ARCH_X86_64_CPU_H
#define ARCH_X86_64_CPU_H

#include <stdint.h>

/*
 * The seven things `kernel/` asks a processor to do.
 *
 * The AArch64 twin of this file exists because sixteen sites across four
 * kernel files each wrote their own `msr daifset`. Writing them as inlines
 * turned "the kernel is portable" from a claim into a compile error - and
 * this file is what collects on that: seven functions, and `kernel/` did not
 * change by one line to gain a second architecture.
 *
 * The instructions have nothing in common with the ARM ones. That is the
 * point; the *interface* is what had to be the same.
 */

struct cpu_info;

void     cpu_identify(struct cpu_info *out);
unsigned cpu_dcache_line(const struct cpu_info *cpu);
unsigned cpu_pa_bits(const struct cpu_info *cpu);

/*
 * Everything off, and what was on before.
 *
 * Saved and restored rather than unconditionally re-enabled, because a
 * caller can be reached from somewhere that already held interrupts off.
 *
 * On x86 there is one flag, IF, and it lives in RFLAGS with everything else
 * - carry, zero, direction - so saving "the interrupt state" means saving
 * the whole register. AArch64 has DAIF, which is only the masks. Same
 * shape, and this one carries more than it means to.
 */
static inline uint64_t cpu_interrupts_save(void)
{
    uint64_t flags;

    __asm__ volatile("pushfq; popq %0" : "=r"(flags) :: "memory");
    __asm__ volatile("cli" ::: "memory");

    return flags;
}

static inline void cpu_interrupts_restore(uint64_t flags)
{
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

/*
 * IRQ alone - which on x86 is the same bit as "everything", because there
 * is no second maskable class.
 *
 * AArch64 distinguishes IRQ from FIQ and `DAIFSet #3` is both where `#2` is
 * IRQ alone; the kernel keeps them separate because merging them would
 * silently change when a fast interrupt may arrive. Here the distinction
 * does not exist: NMI is not maskable at all, and everything else is IF.
 */
static inline void cpu_irq_enable(void)
{
    __asm__ volatile("sti" ::: "memory");
}

static inline void cpu_irq_disable(void)
{
    __asm__ volatile("cli" ::: "memory");
}

/*
 * Sleep until something happens.
 *
 * `hlt` wakes on a pending interrupt, and - exactly like `wfi` - it wakes
 * even when interrupts are masked: masking stops the exception being
 * *taken*, not the wakeup. The idle loop depends on that on both machines.
 *
 * The one difference that matters: `sti` on x86 does not take effect until
 * *after* the next instruction, which makes `sti; hlt` atomic against the
 * interrupt arriving in between. That race is real on ARM and is why the
 * idle loop there unmasks in the order it does.
 */
static inline void cpu_wait_for_interrupt(void)
{
    __asm__ volatile("hlt");
}

/*
 * The cycle counter, with the barrier that makes it mean anything.
 *
 * `lfence` before, for the reason `isb` is there on ARM: without it the
 * read can be reordered ahead of whatever the caller was timing. `rdtsc`
 * answers in EDX:EAX - two halves, always - which is the constraint that
 * caught this project out once already, in `rdmsr`.
 */
static inline uint64_t cpu_cycles(void)
{
    uint32_t lo, hi;

    __asm__ volatile("lfence" ::: "memory");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));

    return ((uint64_t)hi << 32) | lo;
}

/*
 * Which privilege level this is running at.
 *
 * AArch64 has a register that says so. x86 does not: the answer is the low
 * two bits of CS, the ring, and it is 0 for the kernel and 3 for a process.
 * Reported the way `cpu_current_el` reports 1 and 0, so the two say the
 * same thing in the same units - the kernel is the higher number on ARM and
 * the lower one here, and that is the only place the mapping is written
 * down.
 */
static inline unsigned cpu_current_el(void)
{
    uint16_t cs;

    __asm__ volatile("movw %%cs, %0" : "=r"(cs));

    /* ring 0 -> EL1, ring 3 -> EL0, so the kernel is the larger number on
     * both and `cpu_current_el() == 1` means the same thing either side. */
    return ((cs & 3u) == 0u) ? 1u : 0u;
}

#endif /* ARCH_X86_64_CPU_H */
