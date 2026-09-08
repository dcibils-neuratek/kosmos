/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#include "smp.h"

/*
 * Kosmos entry point.
 *
 * The first instructions that run. In order: park every core that is not
 * core 0, get down to EL1 if we were handed something higher, set up the
 * stack, zero .bss, and branch to C.
 *
 * Nothing here may touch the stack before `sp` is set. The reset value of
 * `sp` is 0, so a function prologue that spills a register writes to an
 * unmapped address, takes an exception with no vector installed, and hangs
 * with no output at all. That failure looks exactly like ten other ones.
 */

.section .text.boot, "ax"
.global _start

_start:
    /*
     * Park the secondary cores.
     *
     * QEMU virt starts one core by default and the Pi firmware starts four.
     * Without this, on hardware every core runs kmain and you see the output
     * four times over, interleaved, and none of it makes sense.
     *
     * Aff0 (the low byte of MPIDR_EL1) is the core number within the cluster.
     * Good enough while there is a single cluster; SMP at M6 replaces it.
     */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xff
    cbnz    x0, park

    /*
     * Get to EL1.
     *
     * Which level we are handed depends on the machine, so read it instead
     * of assuming. Measured, not remembered:
     *
     *   -M virt                      EL1
     *   -M virt,virtualization=on    EL2
     *   -M virt,secure=on            EL3
     *
     * The Pi firmware hands off at EL2. Both of the levels Kosmos actually
     * boots from are handled; EL3 is not a configuration this project
     * targets, and getting down from it needs SCR_EL3 and a monitor-mode
     * return that would be dead code. It parks instead of quietly running
     * the whole kernel at EL3, which is what a `b.ne at_el1` here would do.
     */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #1
    b.eq    at_el1                  /* already where we want to be */
    cmp     x0, #2
    b.ne    park                    /* EL3, or something unexpected */

drop_to_el1:
    /* HCR_EL2.RW = 1: EL1 runs AArch64, not AArch32. Leaving this clear is
     * how you end up executing your 64-bit kernel as ARM32 and faulting on
     * the first instruction. */
    movz    x0, #0x8000, lsl #16
    msr     hcr_el2, x0

    /* A known SCTLR_EL1 before entering EL1: MMU off, caches off, little
     * endian, with the architecturally RES1 bits set. The reset value is
     * UNKNOWN on several cores, so it gets written rather than trusted. */
    movz    x0, #0x0800
    movk    x0, #0x30d0, lsl #16
    msr     sctlr_el1, x0

    /* Return into EL1h (M[3:0] = 0b0101, meaning EL1 using SP_EL1) with
     * D, A, I and F masked. There is no exception vector yet, so anything
     * that fires before M1 is a silent hang. */
    movz    x0, #0x3c5
    msr     spsr_el2, x0
    adr     x0, at_el1
    msr     elr_el2, x0
    eret

at_el1:
    /*
     * Let EL1 touch the FP and SIMD registers.
     *
     * CPACR_EL1.FPEN resets to 0b00, which traps every FP and SIMD access at
     * EL0 and EL1 alike, and the trap arrives as EC 0x07 with no hint of
     * what it means. The kernel's own C is built -mgeneral-regs-only and
     * never emits one, but two things here do:
     *
     *   - setjmp and longjmp save and restore d8 to d15, because AAPCS64
     *     makes them callee-saved and Lua raises every error through them.
     *   - Lua's numbers are doubles, so from M2 the interpreter is full of
     *     them.
     *
     * The consequence to keep in view: from M3 there is a context switch,
     * and it does not save FP state. As long as Lua stays on one thread that
     * is fine. The moment it does not, this needs lazy FP save, which the
     * roadmap already schedules at M10 for Doom and which will be needed
     * sooner.
     */
    mov     x0, #(0 << 20)          /* CPACR_EL1.FPEN = 0b00: trap both, lazily */
    msr     cpacr_el1, x0
    isb

    /*
     * Two stacks, and which register holds which is the whole point.
     *
     * EL1 has two stack pointers, SP_EL0 and SP_EL1, and SPSel chooses
     * between them. Taking an exception always sets SPSel to 1. So if the
     * kernel runs on SP_EL0, the handler is handed SP_EL1 by the hardware,
     * with no code to switch anything: a stack overflow faults, and the
     * handler that reports it runs somewhere else and survives to report it.
     *
     * It also makes a double fault legible rather than silent. An exception
     * taken while SPSel is 0 lands in the "current EL, SP_EL0" quarter of the
     * vector table; one taken inside the handler, where SPSel is already 1,
     * lands in the "current EL, SP_ELx" quarter. The dump names the vector,
     * so the two are told apart by which entry fired.
     *
     * We arrive here with SPSel already 1, so this first write is SP_EL1.
     */
    adrp    x0, __exception_stack_top
    add     x0, x0, :lo12:__exception_stack_top
    mov     sp, x0

    /* Now switch to SP_EL0 and give it the kernel's own stack. adrp/add
     * rather than `ldr =sym` so there is no literal pool in the boot
     * section. */
    msr     spsel, #0
    adrp    x0, __stack_top
    add     x0, x0, :lo12:__stack_top
    mov     sp, x0

    /*
     * Zero .bss. C assumes it, and nothing has done it: the section takes no
     * space in the image, so what is there at boot is whatever the firmware
     * left behind.
     *
     * The linker script aligns both ends to 16, so this goes 16 bytes at a
     * time with no tail. `b.hs` and not `b.hi` so an empty .bss (start ==
     * end) writes nothing.
     */
    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start
    adrp    x1, __bss_end
    add     x1, x1, :lo12:__bss_end
1:
    cmp     x0, x1
    b.hs    2f
    stp     xzr, xzr, [x0], #16
    b       1b
2:

    bl      kmain

    /* kmain does not return. If it ever does, park rather than run off the
     * end of the image into whatever follows. */
/*
 * Where a second processor arrives.
 *
 * PSCI `CPU_ON` starts a core here with **the MMU off, caches off, at EL1**,
 * and with the context word this kernel passed waiting in x0 - which is the
 * processor's index. Everything `_start` did for core 0 has to be done
 * again, because a processor has its own copy of every system register even
 * though the page tables are shared.
 *
 * What is *not* done again: the tables, .bss, and anything else that
 * belongs to the machine rather than to a core. Those exist already.
 *
 * **The stack has to be found before the MMU is on**, which is why it is an
 * array indexed by the context word rather than anything cleverer. With
 * translation off every access is non-cacheable, so the handful of stores
 * between here and `mmu_enable_here` are slow and, more importantly, do not
 * share a cache with core 0 - which is safe only because this stack belongs
 * to this core and nothing else will ever look at it. `secondary_main`
 * turns translation on as its first act, and the window is a function
 * prologue.
 */
.global _secondary_start
_secondary_start:
    /* The same known SCTLR_EL1 core 0 writes: MMU off, caches off, little
     * endian, RES1 bits set. The reset value is UNKNOWN on several cores
     * and PSCI does not promise one. */
    movz    x1, #0x0800
    movk    x1, #0x30d0, lsl #16
    msr     sctlr_el1, x1
    isb

    /* Trap FP and SIMD, lazily, exactly as core 0 does. */
    mov     x1, #(0 << 20)
    msr     cpacr_el1, x1
    isb

    /*
     * Two stacks, from the two arrays `kernel/smp.c` declares. x0 is this
     * core's index, and each slot is `SECONDARY_STACK_BYTES`; the label is
     * the *base*, so the top of slot n is base + (n + 1) * size.
     */
    mov     x2, #SECONDARY_STACK_BYTES
    add     x3, x0, #1
    mul     x3, x3, x2

    adrp    x1, secondary_exception_stacks
    add     x1, x1, :lo12:secondary_exception_stacks
    add     x1, x1, x3
    mov     sp, x1                  /* arrives with SPSel 1: this is SP_EL1 */

    msr     spsel, #0
    adrp    x1, secondary_stacks
    add     x1, x1, :lo12:secondary_stacks
    add     x1, x1, x3
    mov     sp, x1                  /* and this is SP_EL0, the kernel's */

    /*
     * Translation on, with the tables core 0 built, before any C runs.
     *
     * **The last step of this architecture's entry path, and that is why it
     * is here rather than in `kernel/smp.c`.** It used to be the first
     * statement of `secondary_main`, which made a generic file call an
     * AArch64 function - and the x86-64 build refused to link the moment
     * that file was added to it. The link error was right: how a newly
     * started core reaches a state where it can execute C is exactly what
     * `arch/` and `boot/` are for, and the two machines do not agree on it.
     * A secondary here arrives with the MMU off; one on x86-64 will arrive
     * from a real-mode trampoline already in long mode, because long mode
     * requires paging.
     *
     * x19 because it is callee-saved: x0 is the index and `secondary_main`
     * wants it back.
     */
    mov     x19, x0
    bl      mmu_enable_here
    mov     x0, x19

    /* x0 is still the index, which is `secondary_main`'s only argument. */
    bl      secondary_main

    /* It does not return. If it ever did, park rather than run off the end
     * of the image into whatever follows - the same reason core 0 does. */
park:
    wfe
    b       park
