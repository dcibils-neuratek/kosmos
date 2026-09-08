/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef ARCH_X86_64_CPU_H
#define ARCH_X86_64_CPU_H

#include <stdbool.h>
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

/*
 * What the board measured the counter to be running at, because this
 * architecture usually cannot say.
 *
 * AArch64 has CNTFRQ_EL0: firmware is required to program it and reading it
 * is the whole of the question. The TSC has no such register. CPUID leaf
 * 0x15 states the rate on recent parts and says nothing on the rest -
 * including under emulation - so the only honest answer is to *measure* it
 * against a clock whose frequency is known, and the only such clock here is
 * a board device.
 *
 * So the board calibrates and tells the architecture, which is the one
 * direction that keeps `cpu.c` free of I/O ports. Ignored when CPUID
 * already answered: a stated rate beats a measured one.
 */
void cpu_set_counter_hz(uint64_t hz);

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


/*
 * Where this core's own state lives - and on this board, not yet in a
 * register.
 *
 * AArch64 has `TPIDR_EL1`: banked, invisible to a process, set once per
 * core at boot. The equivalent here is the `GS` base, and it is not
 * equivalent: x86 has **one** `GS` shared between ring 3 and ring 0, so
 * using it means `swapgs` on every entry and every exit, in `vectors.S` and
 * `user.S`, with the classic hazard that an exception arriving between the
 * two finds the wrong one.
 *
 * That is real surgery on the entry path and it belongs with this board's
 * *second core*, not before it. `docs/smp.md` does AArch64 first for
 * exactly this kind of reason. Until then a static is correct for one
 * processor, and this comment is what stops it being mistaken for the
 * finished thing.
 */
static void *cpu_self_storage;

static inline void cpu_set_self(void *self)
{
    cpu_self_storage = self;
}

static inline void *cpu_self(void)
{
    return cpu_self_storage;
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
 * Whether interrupts are currently taken on this core.
 *
 * RFLAGS.IF, bit 9, and here the bit reads the way it sounds: set means
 * interrupts are enabled. The AArch64 twin is the negation of a mask, which
 * is the trap that one names.
 */
static inline bool cpu_interrupts_enabled(void)
{
    uint64_t flags;

    __asm__ volatile("pushfq; popq %0" : "=r"(flags) :: "memory");

    return (flags & (1UL << 9)) != 0;   /* RFLAGS.IF */
}

/*
 * The one instruction a lock is built from.
 *
 * `xchg` to memory is atomic whether or not it is written with a `lock`
 * prefix - the prefix is implied for this one instruction - and it carries
 * full ordering with it, so there is no separate acquire to write. The
 * prefix is here anyway because a reader should not have to know that.
 *
 * The release is a plain store, and that is not a shortcut: **x86-64 is
 * total-store-ordered**, so every write this core made inside the critical
 * section is already visible before a later store is. What is missing is a
 * promise from the *compiler*, which is what the empty asm with a memory
 * clobber buys - the same bargain `cpu_publish` makes two functions up.
 *
 * The AArch64 twin needs `ldaxr`/`stxr` and a retry loop, because a weakly
 * ordered machine gives neither the atomicity nor the ordering for free.
 * That asymmetry is why `docs/smp.md` does SMP on ARM first.
 */
static inline bool cpu_lock_try(volatile unsigned *word)
{
    unsigned prev = 1u;

    __asm__ volatile("lock xchgl %0, %1"
                     : "+r"(prev), "+m"(*word)
                     :: "memory");

    return prev == 0u;
}

static inline void cpu_lock_release(volatile unsigned *word)
{
    __asm__ volatile("" ::: "memory");
    *word = 0u;
}

/*
 * The two halves of handing a structure to another processor.
 *
 * The AArch64 twin of this explains what they are for and why `volatile`
 * was not enough. Here they are compiler barriers and nothing more, and the
 * reason is the strongest reason there is: **x86-64 is total-store-ordered.**
 * Stores are not reordered with other stores and loads are not reordered
 * with other loads, so the processor already provides both. What it does
 * not provide is any promise from the *compiler*, which is free to sink a
 * store past the flag that publishes it - so the empty asm with a memory
 * clobber is the whole of the work.
 *
 * Written out rather than left absent, because an architecture where the
 * barrier happens to be free is exactly where a missing one is invisible
 * until the code is read on the other board. `docs/smp.md` does SMP on
 * AArch64 first for this class of reason.
 */
static inline void cpu_publish(void)
{
    __asm__ volatile("" ::: "memory");
}

static inline void cpu_observe(void)
{
    __asm__ volatile("" ::: "memory");
}

/*
 * Where a newly started core lands - and on this machine, nowhere yet.
 *
 * `boot/x86_64/start.S` has one entry point and it assumes the boot
 * protocol handed it a machine in a known state. A secondary here does not
 * arrive that way: `INIT`-`SIPI`-`SIPI` starts it in **real mode**, at a
 * page under 1 MB, and the same long-mode climb has to happen again from
 * there. That trampoline does not exist, so this says so and
 * `smp_start_others` stops before asking the board for anything.
 *
 * The board refuses too - `hal/pc/cpu_on.c` returns false, because it has
 * no local APIC to send the sequence with. **Two separate missing things,
 * said separately**, so that building one does not silently look like
 * building both. `docs/smp.md` is where they are counted.
 */
static inline uintptr_t cpu_secondary_entry(void)
{
    return 0;
}

static inline void cpu_relax(void)
{
    __asm__ volatile("pause" ::: "memory");
}

#endif /* ARCH_X86_64_CPU_H */
