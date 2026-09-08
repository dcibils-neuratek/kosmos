/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef ARCH_AARCH64_CPU_H
#define ARCH_AARCH64_CPU_H

#include <stdbool.h>
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


/*
 * Where this core's own state lives.
 *
 * `TPIDR_EL1` is the architecture's answer and it is a good one: a
 * 64-bit scratch register, banked, readable and writable only at EL1. A
 * process cannot see it and cannot change it, so it is set once per core at
 * boot and read from anywhere afterwards - including the first instruction
 * of an exception handler, which is the case that matters and the reason
 * the register exists.
 *
 * **Nothing on the entry path has to do anything.** x86 needs `swapgs` at
 * every boundary because it has one `GS` for both privilege levels; this
 * has two registers and hands the kernel its own. That asymmetry is most of
 * why `docs/smp.md` puts AArch64 first.
 *
 * `kernel/percpu.h` is what this is for and holds the argument.
 */
static inline void cpu_set_self(void *self)
{
    __asm__ volatile("msr tpidr_el1, %0" : : "r"(self));
}

static inline void *cpu_self(void)
{
    void *self;

    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(self));

    return self;
}


/*
 * A hint that this core is spinning and the other one should get on with
 * it.
 *
 * `yield` on AArch64 and `pause` on x86: both tell the processor that this
 * loop is waiting rather than working, which on a machine with hardware
 * threads lets the sibling have the pipeline and on any machine saves
 * power. Neither is a barrier and neither orders anything - the loop still
 * needs whatever it needed.
 */
/*
 * Where a newly started core lands, or 0 if this architecture has nowhere
 * to put one yet.
 *
 * An address rather than a symbol, because `kernel/smp.c` hands it to
 * `hal_cpu_on` and the board is what knows how to start a core - PSCI here,
 * `INIT`-`SIPI`-`SIPI` on the other machine. **Neither of them knows where
 * it should land**, which is this side of the split: the board starts a
 * core, the architecture says what a started core should run.
 *
 * `static inline` so the x86-64 twin - which answers 0 - generates no
 * reference to a symbol its `boot/` does not define. That is the whole
 * reason this is a function and not an `extern char[]` in `smp.c`.
 */
static inline uintptr_t cpu_secondary_entry(void)
{
    extern char _secondary_start[];     /* boot/start.S */

    return (uintptr_t)_secondary_start;
}

static inline void cpu_relax(void)
{
    __asm__ volatile("yield" ::: "memory");
}

/*
 * Whether interrupts are currently taken on this core.
 *
 * Read rather than assumed. `PSTATE.I` set means IRQ is masked, so this is
 * the negation of a bit - written out because "I" reads like "interrupts on"
 * and means the opposite, which is the kind of thing that is right in the
 * head of whoever wrote it and wrong in everybody else's.
 *
 * Exists for one assertion: that a lock really does mask, which is the
 * property the whole locking design rests on and is invisible from anywhere
 * else.
 */
static inline bool cpu_interrupts_enabled(void)
{
    uint64_t daif;

    __asm__ volatile("mrs %0, daif" : "=r"(daif));

    return (daif & (1UL << 7)) == 0;    /* PSTATE.I */
}

/*
 * The one instruction pair a lock is built from.
 *
 * **This is the first atomic in this kernel**, and it arrives with
 * `docs/smp.md` step five - a second core running threads - because until
 * now mutual exclusion here was one sentence: there is one core, and the
 * kernel runs with interrupts masked.
 *
 * `ldaxr` is a load-acquire *exclusive*: it reads the word, takes the
 * exclusive monitor, and orders everything after it. `stxr` stores only if
 * nothing else touched the address in between, and reports whether it won.
 * The pair is how AArch64 spells compare-and-swap, and the retry loop lives
 * in the caller because a failed `stxr` is not a held lock - it is a lost
 * race, and the difference matters for how long you spin.
 *
 * `clrex` on the giving-up path, because a `ldaxr` with no matching store
 * leaves the monitor set and the next unrelated exclusive sequence would
 * inherit it.
 *
 * The release is `stlr`, a store-release: everything this core did inside
 * the critical section is visible before the word reads as free. Not a `dmb`
 * and then a plain store, which is the same thing written less clearly and
 * one instruction slower.
 */
static inline bool cpu_lock_try(volatile unsigned *word)
{
    unsigned prev;
    unsigned failed;

    __asm__ volatile(
        "   ldaxr   %w0, [%2]       \n"
        "   cbnz    %w0, 1f         \n"
        "   stxr    %w1, %w3, [%2]  \n"
        "   b       2f              \n"
        "1: clrex                   \n"
        "   mov     %w1, #1         \n"
        "2:                         \n"
        : "=&r"(prev), "=&r"(failed)
        : "r"(word), "r"(1u)
        : "memory", "cc");

    return (prev == 0u) && (failed == 0u);
}

static inline void cpu_lock_release(volatile unsigned *word)
{
    __asm__ volatile("stlr wzr, [%0]" :: "r"(word) : "memory");
}

/*
 * The two halves of handing a structure to another processor.
 *
 * **`volatile` is not a barrier**, and that is the bug these exist to fix.
 * `kernel/smp.c` had a secondary fill its `struct percpu` and then increment
 * a `volatile unsigned online`, with core 0 spinning on that variable and
 * reading the slot once it rose. `volatile` tells the *compiler* not to cache
 * the variable; it says nothing to the processor about the order the two
 * stores become visible in. On AArch64, which is weakly ordered, core 0 was
 * architecturally allowed to see the count rise before the slot was written -
 * and `cpu: every processor claimed its own slot` would then read a zeroed
 * slot and fail, once in some number of thousands of boots, on a machine
 * nobody was watching.
 *
 * `cpu_publish` is the release: everything this core wrote before it is
 * visible to anyone who observes what it writes after. `dmb ishst` and not
 * `dsb sy` - stores only, inner shareable, which is the whole of what is
 * needed and is what the principle about naming the barrier asks for.
 *
 * `cpu_observe` is the acquire, and it is `dmb ishld`: loads after it cannot
 * be hoisted above the load that satisfied the wait.
 *
 * A pair, always. A release with no matching acquire orders one side of a
 * conversation, which is worth nothing.
 */
static inline void cpu_publish(void)
{
    __asm__ volatile("dmb ishst" ::: "memory");
}

static inline void cpu_observe(void)
{
    __asm__ volatile("dmb ishld" ::: "memory");
}

#endif /* ARCH_AARCH64_CPU_H */
