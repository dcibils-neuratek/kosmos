/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef TESTS_MACHINE_H
#define TESTS_MACHINE_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu.h"

/*
 * The handful of machine operations a kernel test needs and the
 * architecture's own `cpu.h` does not already have.
 *
 * Named `machine.h` rather than `cpu.h` because `-Itests` is on the compile
 * line: a second `cpu.h` here would shadow the architecture's for every
 * file in the build, and the first thing it would shadow is this file's own
 * include of it.
 *
 * **Most of them it does have**, which is why this file is short. Masking
 * interrupts, reading the cycle counter and asking which privilege level
 * this is are already `cpu_irq_disable`, `cpu_cycles` and `cpu_current_el`,
 * and `cpu_current_el` already reports 1 for the kernel on both boards - so
 * a test asking "am I privileged" asks the same question in the same units
 * either side. Those are exactly the sixteen sites the 0.9.0 review counted
 * in `kernel/` and moved behind `arch/`, and the suite gets them for free.
 *
 * What is left is the deliberately awkward: instructions chosen to fault,
 * a store the compiler is not allowed to reason about, and a register held
 * across a context switch. None of those has any business in a kernel header
 * - nothing in the kernel wants to execute an undefined instruction - so
 * they live here, beside the only code that does.
 */

/*
 * An instruction that is not one.
 *
 * `udf` is A64's permanently-undefined encoding and `ud2` is x86's, and
 * both are the architecture's own guarantee rather than a byte pattern that
 * happens to be unallocated today.
 *
 * Used rather than a null dereference for the earliest trap tests, because
 * those run before the MMU exists: with translation off, address 0 on QEMU
 * virt is flash rather than a hole, and a write there does not necessarily
 * fault at all.
 */
#if defined(__aarch64__)
#define TEST_UNDEFINED_INSTRUCTION()    __asm__ volatile("udf #0")
#define TEST_BREAKPOINT()               __asm__ volatile("brk #0")
#elif defined(__x86_64__)
#define TEST_UNDEFINED_INSTRUCTION()    __asm__ volatile("ud2")
#define TEST_BREAKPOINT()               __asm__ volatile("int3")
#else
#error "no undefined instruction for this architecture"
#endif

/*
 * A store the compiler is not allowed to reason about.
 *
 * Writing `*(volatile int *)0 = 1` in C does not work. Dereferencing a null
 * pointer is undefined behaviour, so the compiler is entitled to assume it
 * cannot happen: it emits the store and then treats everything after it as
 * unreachable, appending a trap instruction of its own. The store faults,
 * recovery lands on that trap, and the kernel takes a second fault with the
 * expectation already spent.
 *
 * Found exactly that way. In assembly a store is just a store.
 */
static inline void test_store_to(uintptr_t addr, uint32_t value)
{
#if defined(__aarch64__)
    __asm__ volatile("str %w0, [%1]" : : "r"(value), "r"(addr) : "memory");
#else
    __asm__ volatile("movl %0, (%1)" : : "r"(value), "r"(addr) : "memory");
#endif
}

/* And the read, for the tests about pages that may not be read. */
static inline uint32_t test_load_from(uintptr_t addr)
{
    uint32_t value;

#if defined(__aarch64__)
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(addr) : "memory");
#else
    __asm__ volatile("movl (%1), %0" : "=r"(value) : "r"(addr) : "memory");
#endif

    return value;
}

/* Where the stack is, right here. */
static inline uintptr_t test_stack_pointer(void)
{
    uintptr_t sp;

#if defined(__aarch64__)
    __asm__ volatile("mov %0, sp" : "=r"(sp));
#else
    __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
#endif

    return sp;
}

/*
 * A callee-saved integer register, written and read back.
 *
 * The point of the pair is a context switch between them: whatever the ABI
 * promises to preserve across a call, the switch has to preserve across a
 * thread. `x19` is the first of AArch64's callee-saved set and `rbx` is the
 * first of System V's, and each is named rather than left to the compiler
 * so the test is about a specific register rather than about wherever a
 * value happened to live.
 */
static inline void test_scratch_set(uint64_t value)
{
#if defined(__aarch64__)
    __asm__ volatile("mov x19, %0" : : "r"(value) : "x19");
#else
    __asm__ volatile("movq %0, %%rbx" : : "r"(value) : "rbx");
#endif
}

static inline uint64_t test_scratch_get(void)
{
    uint64_t value;

#if defined(__aarch64__)
    __asm__ volatile("mov %0, x19" : "=r"(value));
#else
    __asm__ volatile("movq %%rbx, %0" : "=r"(value));
#endif

    return value;
}

/*
 * A floating-point register, likewise - and the asymmetry here is real
 * rather than an oversight.
 *
 * `d8` is *callee-saved* on AArch64: AAPCS64 promises the low halves of
 * `d8`-`d15` survive a call, which is why `setjmp` there saves eight
 * doubles and `setjmp-x86_64.S` saves none. **On System V every XMM
 * register is caller-saved**, so nothing promises `xmm8` survives anything,
 * and a test asserting that a voluntary switch preserved it would be
 * asserting something this architecture never said.
 *
 * So this pair is used only where the claim holds on both boards: across a
 * *preemption*, which is not a call and where the kernel's lazy-FP path is
 * what has to get it right. `arch/x86_64/fp.c` and `arch/aarch64/fp.c` both
 * make that promise, and both are what this is aimed at.
 */
static inline void test_fp_scratch_set(double value)
{
#if defined(__aarch64__)
    __asm__ volatile("fmov d8, %d0" : : "w"(value) : "d8");
#else
    __asm__ volatile("movsd %0, %%xmm8" : : "x"(value) : "xmm8");
#endif
}

static inline double test_fp_scratch_get(void)
{
    double value;

#if defined(__aarch64__)
    __asm__ volatile("fmov %d0, d8" : "=w"(value));
#else
    __asm__ volatile("movsd %%xmm8, %0" : "=x"(value));
#endif

    return value;
}

/*
 * A known bit pattern held in an FP register across a spin long enough to
 * be preempted.
 *
 * **The spin is inside the asm block on purpose.** A loop in C around it
 * would be a call boundary, where both ABIs say the register is dead
 * anyway and the bug this is aimed at cannot show. That bug is a context
 * switch that does not carry the floating-point file: thread A leaves a
 * value in the register, thread B runs and uses it, and A comes back with
 * B's number.
 *
 * Two nested loops, because one has to outlast a timer period. A shorter
 * spin finishes inside a quantum, is never preempted, and passes without
 * testing anything - which is what the first version of this did.
 *
 * The register differs and the claim does not. `d0` is AArch64's first
 * FP argument register and `xmm0` is System V's, and neither is preserved
 * across a *call* on its board - which is the point: this is about a
 * preemption, where the kernel is what has to get it right. ARM does it
 * lazily through a trap and x86 does it eagerly in `switch.S`, and this
 * test cannot tell which, which is exactly what a test of the guarantee
 * rather than the mechanism should look like.
 */
#if defined(__aarch64__)
#define TEST_FP_SPIN(got, want)                                             \
    __asm__ volatile(                                                       \
        "fmov   d0, %1\n"                                                   \
        "mov    x10, #24\n"                                                 \
        "1:\n"                                                              \
        "mov    x9, #0xffff\n"                                              \
        "2:\n"                                                              \
        "subs   x9, x9, #1\n"                                               \
        "b.ne   2b\n"                                                       \
        "subs   x10, x10, #1\n"                                             \
        "b.ne   1b\n"                                                       \
        "fmov   %0, d0\n"                                                   \
        : "=r"(got)                                                         \
        : "r"(want)                                                         \
        : "d0", "x9", "x10", "cc")
#else
#define TEST_FP_SPIN(got, want)                                             \
    __asm__ volatile(                                                       \
        "movq   %1, %%xmm0\n"                                               \
        "movq   $24, %%r10\n"                                               \
        "1:\n"                                                              \
        "movq   $0xffff, %%r9\n"                                            \
        "2:\n"                                                              \
        "decq   %%r9\n"                                                     \
        "jnz    2b\n"                                                       \
        "decq   %%r10\n"                                                    \
        "jnz    1b\n"                                                       \
        "movq   %%xmm0, %0\n"                                               \
        : "=r"(got)                                                         \
        : "r"(want)                                                         \
        : "xmm0", "r9", "r10", "cc")
#endif

/*
 * Are interrupts arriving?
 *
 * `cpu_interrupts_save` hands back the whole word and also *masks*, which
 * is the wrong shape for a test that only wants to look. DAIF bit 7 is
 * AArch64's I mask - set means masked - and RFLAGS bit 9 is x86's IF, which
 * is the other way round: set means enabled. Both are read here so that the
 * inversion is written down once rather than at every call site.
 */
static inline bool test_interrupts_enabled(void)
{
#if defined(__aarch64__)
    uint64_t daif;

    __asm__ volatile("mrs %0, daif" : "=r"(daif));

    return (daif & (1UL << 7)) == 0;
#else
    uint64_t flags;

    __asm__ volatile("pushfq; popq %0" : "=r"(flags));

    return (flags & (1UL << 9)) != 0;
#endif
}

/*
 * The argument slot a not-yet-started thread will read.
 *
 * `context_init` plants the entry function and its argument in the first
 * two callee-saved registers, because those are the first two slots of the
 * saved context and so the easiest to reach - `x19` and `x20` on one board,
 * `rbx` and `r12` on the other. A test that has to hand a thread something
 * *after* creating it and before waking it writes the second of those, and
 * this is the only place that has to know which register it is.
 */
#include "context.h"

static inline void test_context_set_arg(struct context *ctx, uint64_t arg)
{
#if defined(__aarch64__)
    ctx->x20 = arg;
#else
    ctx->r12 = arg;
#endif
}

/*
 * What a page-table entry says, asked the same way on both boards.
 *
 * Neither the address field nor the permission bits survive the crossing.
 * AArch64 puts the frame in bits 47:12 and spends two bits on an access
 * permission that names both privilege levels at once - `AP=11` is "EL1
 * read, EL0 read". x86 puts the frame in the same place by coincidence and
 * spells permission as three separate bits: present, writable, and
 * user-accessible, where "read-only to a process" is present and user and
 * *not* writable.
 *
 * So a test asserting either encoding would be asserting about a board.
 * What both are asked here is the property the kernel actually promises:
 * this page is mapped, a process may read it, and no process may write it -
 * which is what makes one physical copy of a program's code safe to share
 * between every address space that runs it.
 */
#include "mmu.h"

static inline uint64_t test_pte_frame(uint64_t entry)
{
#if defined(__aarch64__)
    return entry & DESC_ADDR_MASK;
#else
    return entry & PTE_ADDR_MASK;
#endif
}

static inline bool test_pte_is_mapped(uint64_t entry)
{
    /* Bit 0 is "valid" on one and "present" on the other, and for once the
     * two architectures agree on both the bit and the meaning. */
    return (entry & 1) != 0;
}

static inline bool test_pte_is_read_only_to_a_process(uint64_t entry)
{
#if defined(__aarch64__)
    return (entry & ATTR_AP_RO_EL0) == ATTR_AP_RO_EL0;
#else
    return (entry & PTE_US) != 0 && (entry & PTE_RW) == 0;
#endif
}

/*
 * How fast `cpu_cycles` counts.
 *
 * `CNTFRQ_EL0` is a register the firmware sets and every AArch64 machine
 * has. x86 has no such register in general: the TSC's rate is either in a
 * CPUID leaf or has to be measured against a device whose rate is known,
 * which is what `arch/x86_64/cpu.c` does at boot and caches. Asked through
 * `cpu_identify` on both, so the test does not have to know which.
 *
 * Cached, because on x86 this is a CPUID and the timer tests read it inside
 * a loop bound.
 */
static inline uint64_t test_counter_hz(void)
{
    static uint64_t hz;

    if (hz == 0) {
        struct cpu_info cpu;

        cpu_identify(&cpu);
        hz = cpu.counter_hz;
    }

    return hz;
}

#endif /* TESTS_MACHINE_H */
