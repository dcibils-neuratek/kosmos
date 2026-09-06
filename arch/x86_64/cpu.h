/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef ARCH_X86_64_CPU_H
#define ARCH_X86_64_CPU_H

#include <stdint.h>

/*
 * Which processor this is, asked of the processor.
 *
 * `arch/` is "which CPU are you" and this is the most literal reading of
 * it. Everything here comes out of CPUID, which every x86-64 implements
 * because long mode requires it, so this file needs no board knowledge at
 * all - the same claim `arch/aarch64/cpu.h` makes about its system
 * registers.
 *
 * **The raw words travel with the decode**, everywhere they are shown, for
 * the reason the ARM file gives: a table of part numbers goes stale the
 * moment a part ships that is not in it, and a reader who can see the words
 * can look them up.
 *
 * Intel SDM volume 2A, the CPUID instruction; AMD APM volume 3, appendix E.
 */

/* Which word is which, for whoever decodes them. `syscall.h` says why the
 * kernel does not: the same eight words mean something else on the other
 * machine, and `cpu_arch` is what tells them apart. */
#define CPU_RAW_VENDOR0   0     /* CPUID.0: EBX and EDX, the vendor string */
#define CPU_RAW_VENDOR1   1     /* CPUID.0: ECX, and the highest leaf */
#define CPU_RAW_SIGNATURE 2     /* CPUID.1 EAX: family, model, stepping */
#define CPU_RAW_BRAND     3     /* CPUID.1 EBX: cache line, APIC id */
#define CPU_RAW_FEAT1_ECX 4
#define CPU_RAW_FEAT1_EDX 5
#define CPU_RAW_FEAT7_EBX 6
#define CPU_RAW_FEAT7_ECX 7
#define CPU_RAW_ADDRESS   8     /* CPUID.80000008 EAX: address widths */

struct cpu_info {
    /* Raw, exactly as read. */
    uint64_t vendor0, vendor1;
    uint64_t signature;
    uint64_t brand;                 /* CPUID.1 EBX, where the cache line is */
    uint64_t feat1_ecx, feat1_edx;
    uint64_t feat7_ebx, feat7_ecx;
    uint64_t address;
    uint64_t counter_hz;

    /* Decoded from the signature, with the extended fields folded in the
     * way the manuals specify - which is not the same for the two of them
     * and is the part people get wrong. */
    unsigned family;
    unsigned model;
    unsigned stepping;

    /*
     * The same processor in the words every architecture can say. See
     * `arch/aarch64/cpu.h`, which explains why these exist.
     *
     * `vendor_name` is the twelve-character vendor string, from EBX, EDX
     * and ECX in that order - which is not the order they are returned in,
     * and is the single most copied-wrong line in every x86 identification
     * routine. Held rather than pointed at, because it is built from
     * registers rather than found in a table.
     */
    char        vendor_name[16];
    const char *model_name;
    char        revision_text[16];
    const char *id_name;
    uint64_t    id;
};

void cpu_identify(struct cpu_info *out);

unsigned cpu_arch(void);
unsigned cpu_raw(const struct cpu_info *cpu, uint64_t *out, unsigned max);

/* The cache line, from CPUID.1 EBX[15:8], which counts eight-byte units. */
unsigned cpu_dcache_line(const struct cpu_info *cpu);

/* The physical address range the core can drive, in bits. */
unsigned cpu_pa_bits(const struct cpu_info *cpu);

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
/*
 * Sleep until something happens, and **`sti` is part of the instruction**.
 *
 * This is the sharpest difference between the two processors in this
 * header, and it is a difference in what "masked" means.
 *
 * AArch64's `wfi` wakes on a pending interrupt even with PSTATE.I set -
 * masking stops the exception being *taken*, not the wakeup - so the idle
 * loop in `kernel/main.c` masks interrupts across the check and the sleep,
 * exactly to close the race where an interrupt lands between "nothing is
 * runnable" and "sleep". Its comment says so.
 *
 * `hlt` has no such property. With IF clear it halts and nothing wakes it,
 * ever. The same idle loop, unchanged, is a machine that boots, prints its
 * prompt, and stops - which is what it did: twenty timer interrupts in the
 * first two tenths of a second and then silence, with the shell blocked on
 * a console server blocked on a timeout that could no longer expire.
 *
 * **`sti; hlt` is the answer and it is one instruction pair on purpose.**
 * `sti` does not take effect until after the instruction that follows it -
 * the interrupt shadow - so there is no window between enabling and
 * halting for an interrupt to be missed in. It is the reason the shadow
 * exists, and every x86 kernel's idle loop is these two instructions.
 *
 * The caller unmasks again afterwards, which is now redundant rather than
 * wrong; leaving the caller alone is what keeps `kernel/` free of
 * architecture.
 */
static inline void cpu_wait_for_interrupt(void)
{
    __asm__ volatile("sti; hlt");
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
