#ifndef ARCH_AARCH64_CPU_H
#define ARCH_AARCH64_CPU_H

#include <stdint.h>

/*
 * Which processor this is, asked of the processor.
 *
 * `arch/` is "which CPU are you" and this is the most literal possible
 * reading of that. Every value here comes out of a system register the core
 * implements because the architecture requires it, so this file works on any
 * AArch64 part and needs no board knowledge at all.
 *
 * The bit positions are from Linux's `arch/arm64/tools/sysreg`, which is the
 * machine-readable description the kernel generates its own accessors from,
 * and the part numbers from `arch/arm64/include/asm/cputype.h`. Neither was
 * written from memory: a decode that is subtly wrong prints a plausible
 * processor that is not the one you have, which is worse than printing
 * nothing.
 *
 * **The raw registers travel with the decode**, everywhere they are shown.
 * A table of part numbers goes stale the moment a part ships that is not in
 * it, and a reader who can see MIDR_EL1 can look it up; a reader who can
 * only see "unknown" cannot.
 */

struct cpu_info {
    /* Raw, exactly as read. */
    uint64_t midr;              /* who this core is */
    uint64_t mpidr;             /* where it is in the topology */
    uint64_t ctr;               /* cache geometry */
    uint64_t pfr0;              /* processor features: FP, SIMD, GIC */
    uint64_t isar0;             /* instruction set: AES, SHA, CRC32, atomics */
    uint64_t mmfr0;             /* memory model: physical address range */
    uint64_t counter_hz;        /* CNTFRQ_EL0 */

    /* Decoded from MIDR. */
    unsigned implementer;
    unsigned variant;
    unsigned architecture;
    unsigned part;
    unsigned revision;

    const char *implementer_name;   /* never NULL; "unknown" if not in the table */
    const char *part_name;          /* never NULL */

    /*
     * The same processor, said in words every architecture can say.
     *
     * `kernel/main.c` prints one line naming the machine it woke up on, and
     * it used to print `cpu.implementer_name`, `cpu.part_name` and the
     * literal string "MIDR_EL1" - which made the boot log the last place in
     * `kernel/` that knew which processor it was written for.
     *
     * So the architecture composes its own identity and the kernel prints
     * it. `revision` is a string rather than two numbers because "r0p3" is
     * an ARM convention and a stepping is not: what the two have in common
     * is that there is a short way to say which version of the part this
     * is, not how it is spelled.
     *
     * `vendor_name` is held rather than pointed at. On the other machine it
     * is built from three registers, so a pointer would have to be into
     * this struct - and a struct containing a pointer to itself is one
     * assignment away from a dangling one.
     */
    char        vendor_name[16];    /* "Arm" */
    const char *model_name;         /* never NULL; a static string */
    char        revision_text[16];  /* "r0p3" */
    const char *id_name;            /* "MIDR_EL1" */
    uint64_t    id;                 /* the register that name refers to */
};

void cpu_identify(struct cpu_info *out);

/*
 * The raw registers above, in the order `struct sysinfo` carries them, and
 * which architecture they belong to.
 *
 * The struct above keeps its names because `cpu.c` decodes from them and a
 * name is what makes that readable. What crosses into `kernel/` is a
 * numbered list, because the kernel has no business knowing that word three
 * is called PFR0 - and on the other machine it is not.
 *
 * These indices are this architecture's, and userland reading /dev/cpu uses
 * the same ones after checking `cpu_arch`.
 */
#define CPU_RAW_MIDR    0
#define CPU_RAW_MPIDR   1
#define CPU_RAW_CTR     2
#define CPU_RAW_PFR0    3
#define CPU_RAW_ISAR0   4
#define CPU_RAW_MMFR0   5

unsigned cpu_arch(void);
unsigned cpu_raw(const struct cpu_info *cpu, uint64_t *out, unsigned max);

/* Cache line sizes in bytes, from CTR_EL0. Both are logged as log2 of the
 * number of *words*, which is the encoding people get wrong. */
unsigned cpu_dcache_line(const struct cpu_info *cpu);

/* The physical address range the core can drive, in bits. */
unsigned cpu_pa_bits(const struct cpu_info *cpu);

/*
 * ---------------------------------------------------------------------
 * What the CPU *does*, as opposed to what it is.
 * ---------------------------------------------------------------------
 *
 * Sixteen sites in `kernel/` used to write these instructions out inline,
 * across four files, in 8656 lines that are otherwise portable. Every one
 * is here now, which is what makes the claim "the kernel is not
 * architecture-specific" checkable rather than aspirational: a second
 * `arch/` implements this header and `kernel/` does not change.
 *
 * That second `arch/` is expected - x86-64 - and it is why this is worth
 * doing while it is sixteen sites. `hal.md` records the reasoning.
 *
 * **The two maskings are different and are kept different.** `DAIFSet`'s
 * immediate is a bitmask - D, A, I, F from bit 3 down - so `#3` is I and F
 * together and `#2` is I alone. The scheduler wants everything off while it
 * moves threads between queues; the idle loop wants IRQ off around a `wfi`
 * and nothing else. Collapsing them into one function would have been a
 * silent change to when a fast interrupt can arrive.
 */

/*
 * Everything off, and what was on before.
 *
 * Saved and restored rather than unconditionally re-enabled, because both
 * callers can be reached from somewhere that already held interrupts off.
 */
static inline uint64_t cpu_interrupts_save(void)
{
    uint64_t daif;

    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #3" ::: "memory");

    return daif;
}

static inline void cpu_interrupts_restore(uint64_t daif)
{
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
}

/* IRQ alone, which is what the idle loop and the first enable after boot
 * are talking about. */
static inline void cpu_irq_enable(void)
{
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

static inline void cpu_irq_disable(void)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

/*
 * Sleep until something happens.
 *
 * Wakes on a pending interrupt even with PSTATE.I set: masking stops the
 * exception being *taken*, not the wakeup. The idle loop depends on exactly
 * that, and the comment there explains why the unmask comes after.
 */
static inline void cpu_wait_for_interrupt(void)
{
    __asm__ volatile("wfi");
}

/*
 * The cycle counter, with the barrier that makes it mean anything.
 *
 * `isb` first, or the read can be reordered ahead of whatever the caller
 * was timing. The read is cheap; the barrier is the part that earns it.
 */
static inline uint64_t cpu_cycles(void)
{
    uint64_t t;

    __asm__ volatile("isb" ::: "memory");
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));

    return t;
}

/* Which exception level this is running at, decoded. */
static inline unsigned cpu_current_el(void)
{
    uint64_t el;

    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));

    return (unsigned)((el >> 2) & 3);
}

#endif /* ARCH_AARCH64_CPU_H */
