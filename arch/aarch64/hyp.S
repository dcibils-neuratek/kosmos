/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#include "hyp.h"

/*
 * **Nebula's piece of EL2** - `docs/virtualization.md` step V1.
 *
 * Everything here runs at EL2 with its translation off, which on this
 * board's identity map means at the same addresses the kernel uses. It is
 * entered three ways and does three things:
 *
 * - **`hyp_install`, at boot**, on each core that arrives at EL2: the
 *   vector, a stack, and the EL2 settings EL1 needs to run as it always has
 *   - the counter and its timer, the GIC's system registers, no FP traps.
 *   Then `boot/start.S` drops to EL1 as before.
 * - **An `hvc #0` from the kernel**: `HYP_PING` answers, and `HYP_RUN`
 *   enters a guest.
 * - **The guest stopping**: anything EL2 traps while a guest runs - an
 *   `hvc`, a stage-2 fault, the host's own interrupt - puts the host back
 *   exactly as it called and returns from its `hvc` with why.
 *
 * `TPIDR_EL2` says which of the last two it is: the guest's `hyp_vcpu`
 * while one runs, zero while the host does.
 *
 * **What this does not do yet, and must before a real board** (the Pi 5,
 * or any core with caches that are not QEMU's): EL2's own translation. With
 * it off, EL2 reads and writes memory uncached, and the `hyp_vcpu` the
 * kernel wrote through its caches is not guaranteed to be what EL2 reads.
 * QEMU models no caches, so the gate cannot see the difference; the answer
 * is an identity map for EL2 too, which is `roadmap.md`'s, under Sandboxes.
 */

/* HCR_EL2 while the host runs is RW alone - EL1 is AArch64 - and is
 * written as `movz #0x8000, lsl #16` where it is needed. */
/*
 * And while a guest does: RW, TSC (its SMC to EL2), AMO IMO FMO (every
 * physical interrupt to EL2, so a guest that never stops is still stopped
 * by the host's tick), SWIO, TWI (a guest waiting for an interrupt stops,
 * so its monitor can wait for it rather than the core spinning in the
 * guest), and VM - the second stage of translation on.
 */
#define HCR_GUEST_LO    0x203b          /* ... and TWI: its `wfi` stops it */
#define HCR_GUEST_HI    0x8008
/* CPTR_EL2: the RES1 bits and no traps, for both - a guest's FP registers
 * are in the hardware while it runs, put there by `fp_lend` (`fp.c`). */
#define CPTR_HOST       0x33ff
#define CPTR_GUEST      0x33ff
/* CNTHCTL_EL2: EL1PCTEN and EL1PCEN for the host; a guest reads the
 * counter but its use of the physical timer - the host's - stops it. */
#define CNTHCTL_HOST    3
#define CNTHCTL_GUEST   1

/* GICv3's EL2 registers, by encoding: older assemblers lack the names. */
#define ICC_SRE_EL2     S3_4_C12_C9_5
#define ICH_HCR_EL2     S3_4_C12_C11_0
#define ICH_VMCR_EL2    S3_4_C12_C11_7
#define ICH_LR0_EL2     S3_4_C12_C12_0
#define ICH_LR1_EL2     S3_4_C12_C12_1
#define ICH_LR2_EL2     S3_4_C12_C12_2
#define ICH_LR3_EL2     S3_4_C12_C12_3

/*
 * The 23 EL1 registers a guest and the host each keep, in `hyp.h`'s order,
 * two at a time. `base` points at the first; x3 and x4 are spent.
 */
.macro save_el1 base
    mrs     x3, sctlr_el1
    mrs     x4, ttbr0_el1
    stp     x3, x4, [\base, #0]
    mrs     x3, ttbr1_el1
    mrs     x4, tcr_el1
    stp     x3, x4, [\base, #16]
    mrs     x3, mair_el1
    mrs     x4, amair_el1
    stp     x3, x4, [\base, #32]
    mrs     x3, vbar_el1
    mrs     x4, contextidr_el1
    stp     x3, x4, [\base, #48]
    mrs     x3, tpidr_el1
    mrs     x4, sp_el1
    stp     x3, x4, [\base, #64]
    mrs     x3, elr_el1
    mrs     x4, spsr_el1
    stp     x3, x4, [\base, #80]
    mrs     x3, esr_el1
    mrs     x4, far_el1
    stp     x3, x4, [\base, #96]
    mrs     x3, afsr0_el1
    mrs     x4, afsr1_el1
    stp     x3, x4, [\base, #112]
    mrs     x3, cpacr_el1
    mrs     x4, cntkctl_el1
    stp     x3, x4, [\base, #128]
    mrs     x3, sp_el0
    mrs     x4, tpidr_el0
    stp     x3, x4, [\base, #144]
    mrs     x3, tpidrro_el0
    mrs     x4, par_el1
    stp     x3, x4, [\base, #160]
    mrs     x3, csselr_el1
    str     x3, [\base, #176]
.endm

.macro load_el1 base
    ldp     x3, x4, [\base, #0]
    msr     sctlr_el1, x3
    msr     ttbr0_el1, x4
    ldp     x3, x4, [\base, #16]
    msr     ttbr1_el1, x3
    msr     tcr_el1, x4
    ldp     x3, x4, [\base, #32]
    msr     mair_el1, x3
    msr     amair_el1, x4
    ldp     x3, x4, [\base, #48]
    msr     vbar_el1, x3
    msr     contextidr_el1, x4
    ldp     x3, x4, [\base, #64]
    msr     tpidr_el1, x3
    msr     sp_el1, x4
    ldp     x3, x4, [\base, #80]
    msr     elr_el1, x3
    msr     spsr_el1, x4
    ldp     x3, x4, [\base, #96]
    msr     esr_el1, x3
    msr     far_el1, x4
    ldp     x3, x4, [\base, #112]
    msr     afsr0_el1, x3
    msr     afsr1_el1, x4
    ldp     x3, x4, [\base, #128]
    msr     cpacr_el1, x3
    msr     cntkctl_el1, x4
    ldp     x3, x4, [\base, #144]
    msr     sp_el0, x3
    msr     tpidr_el0, x4
    ldp     x3, x4, [\base, #160]
    msr     tpidrro_el0, x3
    msr     par_el1, x4
    ldr     x3, [\base, #176]
    msr     csselr_el1, x3
.endm

.text

/*
 * hyp_install(x0 = this core's index), at EL2, translation off, from
 * `boot/start.S`. Spends x1 to x3 and returns; the caller's stack is not
 * touched, because at this point there may not be one.
 */
.global hyp_install
hyp_install:
    adrp    x1, hyp_vectors
    add     x1, x1, :lo12:hyp_vectors
    msr     vbar_el2, x1

    /* A stack of its own, at EL2's own stack pointer. */
    msr     spsel, #1
    adrp    x1, hyp_stacks
    add     x1, x1, :lo12:hyp_stacks
    mov     x2, #HYP_STACK_BYTES
    add     x3, x0, #1
    madd    x1, x3, x2, x1
    mov     sp, x1

    msr     tpidr_el2, xzr

    /* What EL1 reads as its own identity: the core's real one. Reset
     * leaves both UNKNOWN once EL2 exists. */
    mrs     x1, midr_el1
    msr     vpidr_el2, x1
    mrs     x1, mpidr_el1
    msr     vmpidr_el2, x1

    /* The counter and the physical timer, EL1's to use; no offset. */
    mov     x1, #CNTHCTL_HOST
    msr     cnthctl_el2, x1
    msr     cntvoff_el2, xzr

    /* Nothing of EL1's trapped: FP and SIMD, the coprocessor space, and
     * every performance counter the core has (HPMN = PMCR_EL0.N). */
    mov     x1, #CPTR_HOST
    msr     cptr_el2, x1
    msr     hstr_el2, xzr
    mrs     x1, pmcr_el0
    ubfx    x1, x1, #11, #5
    msr     mdcr_el2, x1

    /* The GIC's system registers for EL1 (SRE and Enable), when the core
     * has a GICv3 interface at all; the virtual interface off. */
    mrs     x1, id_aa64pfr0_el1
    ubfx    x1, x1, #24, #4
    cbz     x1, 1f
    adrp    x2, hyp_gic
    mov     x3, #1
    strb    w3, [x2, :lo12:hyp_gic]
    mrs     x1, ICC_SRE_EL2
    orr     x1, x1, #0x1               /* SRE */
    orr     x1, x1, #0x8               /* Enable: EL1 may use SRE */
    msr     ICC_SRE_EL2, x1
    isb
    msr     ICH_HCR_EL2, xzr
1:
    /*
     * The second stage's shape, for every guest: a 39-bit guest physical
     * space (T0SZ 25) walked from level 1 (SL0 1) in 4 KB pages, the tables
     * cached write-back and inner shareable, and the output as wide as the
     * core's physical addresses (PS from ID_AA64MMFR0_EL1.PARange). Bit 31
     * is RES1.
     */
    mrs     x1, id_aa64mmfr0_el1
    and     x1, x1, #7
    cmp     x1, #5
    b.ls    2f
    mov     x1, #5
2:
    lsl     x1, x1, #16
    movz    x2, #0x3559
    movk    x2, #0x8000, lsl #16
    orr     x1, x1, x2
    msr     vtcr_el2, x1
    msr     vttbr_el2, xzr

    movz    x1, #0x8000, lsl #16        /* HCR_HOST */
    msr     hcr_el2, x1

    /* EL2's own SCTLR: translation and caches off, little endian, the
     * RES1 bits set. */
    movz    x1, #0x0830
    movk    x1, #0x30c5, lsl #16
    msr     sctlr_el2, x1

    /* This core can enter a guest. */
    adrp    x1, hyp_ready
    add     x1, x1, :lo12:hyp_ready
    mov     x2, #1
    strb    w2, [x1, x0]
    isb
    ret

/* unsigned long hyp_call(unsigned long function) */
.global hyp_call
hyp_call:
    hvc     #0
    ret

/*
 * unsigned long hyp_run(struct hyp_vcpu *vcpu)
 *
 * x19 to x30 come back as they went, because EL2 keeps them in the vcpu;
 * x1 to x18 do not, which the procedure call standard allows a call.
 */
.global hyp_run
hyp_run:
    mov     x1, x0
    mov     x0, #HYP_RUN
    hvc     #0
    ret

/*
 * The vector. 2 KB aligned; sixteen entries of 128 bytes - from EL2 itself
 * with SP_EL0 and with SP_EL2, then from a lower level in AArch64 and in
 * AArch32 - each a sync, an IRQ, an FIQ and an SError.
 */
.balign 2048
hyp_vectors:
    .balign 128
    b       hyp_stuck               /* EL2, SP_EL0 */
    .balign 128
    b       hyp_stuck
    .balign 128
    b       hyp_stuck
    .balign 128
    b       hyp_stuck
    .balign 128
    b       hyp_stuck               /* EL2, SP_EL2: a fault in this file */
    .balign 128
    b       hyp_stuck
    .balign 128
    b       hyp_stuck
    .balign 128
    b       hyp_stuck
    .balign 128
    b       hyp_lower_sync          /* EL1 or EL0, AArch64 */
    .balign 128
    b       hyp_lower_irq
    .balign 128
    b       hyp_lower_fiq
    .balign 128
    b       hyp_lower_serror
    .balign 128
    b       hyp_stuck               /* AArch32: never allowed (HCR.RW) */
    .balign 128
    b       hyp_stuck
    .balign 128
    b       hyp_stuck
    .balign 128
    b       hyp_stuck

/*
 * Something went wrong in EL2 itself. There is nobody to tell: its syndrome
 * is kept for a debugger and the core stays here, which on a machine with a
 * watchdog of any kind is noticed.
 */
hyp_stuck:
    mrs     x0, esr_el2
    adrp    x1, hyp_stuck_esr
    str     x0, [x1, :lo12:hyp_stuck_esr]
1:
    wfe
    b       1b

hyp_lower_sync:
    stp     x0, x1, [sp, #-16]!
    mrs     x0, tpidr_el2
    cbnz    x0, 1f

    /* The host: an `hvc #0`, or something that should never come here. */
    mrs     x1, esr_el2
    lsr     x1, x1, #26
    cmp     x1, #0x16
    b.ne    hyp_stuck
    ldp     x0, x1, [sp], #16
    cmp     x0, #HYP_RUN
    b.eq    enter_guest
    cbnz    x0, 2f
    movz    x0, #0x4f53                 /* HYP_PING_ANSWER, "KOSMOS" */
    movk    x0, #0x534d, lsl #16
    movk    x0, #0x4b4f, lsl #32
    eret
2:
    movn    x0, #0                      /* HYP_UNKNOWN */
    eret
1:
    mov     x1, #HYP_EXIT_SYNC
    b       guest_exit

hyp_lower_irq:
    stp     x0, x1, [sp, #-16]!
    mrs     x0, tpidr_el2
    cbz     x0, host_interrupt
    mov     x1, #HYP_EXIT_IRQ
    b       guest_exit

hyp_lower_fiq:
    stp     x0, x1, [sp, #-16]!
    mrs     x0, tpidr_el2
    cbz     x0, host_interrupt
    mov     x1, #HYP_EXIT_FIQ
    b       guest_exit

hyp_lower_serror:
    stp     x0, x1, [sp, #-16]!
    mrs     x0, tpidr_el2
    cbz     x0, host_interrupt
    mov     x1, #HYP_EXIT_SERROR
    b       guest_exit

/* The host's own interrupts are routed to EL1 (HCR.IMO clear while it
 * runs), so this is never reached; if it were, going back is right. */
host_interrupt:
    ldp     x0, x1, [sp], #16
    eret

/*
 * x1: the vcpu. The host's x19 to x30, its return and its EL1 registers
 * put aside; the guest's EL1 registers, its stage 2 and its traps put in
 * place; its general registers last, x1 the very last.
 */
enter_guest:
    add     x2, x1, #HYP_OFF_HOST_GP
    stp     x19, x20, [x2, #0]
    stp     x21, x22, [x2, #16]
    stp     x23, x24, [x2, #32]
    stp     x25, x26, [x2, #48]
    stp     x27, x28, [x2, #64]
    stp     x29, x30, [x2, #80]
    mrs     x3, elr_el2
    mrs     x4, spsr_el2
    add     x2, x1, #HYP_OFF_HOST_ELR
    stp     x3, x4, [x2]

    add     x2, x1, #HYP_OFF_HOST_SYS
    save_el1 x2
    add     x2, x1, #HYP_OFF_SYS
    load_el1 x2

    ldr     x3, [x1, #HYP_OFF_VTTBR]
    msr     vttbr_el2, x3
    isb
    /* A guest whose tables are new or changed: its VMID's translations
     * dropped, this core only - a vCPU runs where its thread does. */
    ldr     x3, [x1, #HYP_OFF_FLUSH]
    cbz     x3, 1f
    tlbi    vmalls12e1
    dsb     nsh
    str     xzr, [x1, #HYP_OFF_FLUSH]
1:
    /* Its virtual timer, as it left it. */
    ldr     x3, [x1, #HYP_OFF_CNTV_CVAL]
    msr     cntv_cval_el0, x3
    ldr     x3, [x1, #HYP_OFF_CNTV_CTL]
    msr     cntv_ctl_el0, x3

    /* Its interrupts: the list registers and its interface's state, and
     * the virtual interface on - where the core has a GICv3 interface. */
    adrp    x3, hyp_gic
    ldrb    w3, [x3, :lo12:hyp_gic]
    cbz     w3, 2f
    add     x5, x1, #HYP_OFF_LR         /* past ldp's reach from x1 */
    ldp     x3, x4, [x5]
    msr     ICH_LR0_EL2, x3
    msr     ICH_LR1_EL2, x4
    ldp     x3, x4, [x5, #16]
    msr     ICH_LR2_EL2, x3
    msr     ICH_LR3_EL2, x4
    ldr     x3, [x1, #HYP_OFF_VMCR]
    msr     ICH_VMCR_EL2, x3
    mov     x3, #1                      /* En */
    msr     ICH_HCR_EL2, x3
2:
    movz    x3, #HCR_GUEST_LO
    movk    x3, #HCR_GUEST_HI, lsl #16
    msr     hcr_el2, x3
    mov     x3, #CPTR_GUEST
    msr     cptr_el2, x3
    mov     x3, #CNTHCTL_GUEST
    msr     cnthctl_el2, x3
    msr     tpidr_el2, x1

    ldr     x3, [x1, #HYP_OFF_PC]
    ldr     x4, [x1, #HYP_OFF_PSTATE]
    msr     elr_el2, x3
    msr     spsr_el2, x4
    isb

    ldp     x2, x3, [x1, #16]
    ldp     x4, x5, [x1, #32]
    ldp     x6, x7, [x1, #48]
    ldp     x8, x9, [x1, #64]
    ldp     x10, x11, [x1, #80]
    ldp     x12, x13, [x1, #96]
    ldp     x14, x15, [x1, #112]
    ldp     x16, x17, [x1, #128]
    ldp     x18, x19, [x1, #144]
    ldp     x20, x21, [x1, #160]
    ldp     x22, x23, [x1, #176]
    ldp     x24, x25, [x1, #192]
    ldp     x26, x27, [x1, #208]
    ldp     x28, x29, [x1, #224]
    ldr     x30, [x1, #240]
    ldr     x0, [x1, #0]
    ldr     x1, [x1, #8]
    eret

/*
 * x0: the vcpu, x1: why; the guest's x0 and x1 on EL2's stack. Everything
 * of the guest's kept in the vcpu, everything of the host's put back, and
 * the host's `hvc` returns the reason.
 */
guest_exit:
    str     x1, [x0, #HYP_OFF_EXIT]
    stp     x2, x3, [x0, #16]
    stp     x4, x5, [x0, #32]
    stp     x6, x7, [x0, #48]
    stp     x8, x9, [x0, #64]
    stp     x10, x11, [x0, #80]
    stp     x12, x13, [x0, #96]
    stp     x14, x15, [x0, #112]
    stp     x16, x17, [x0, #128]
    stp     x18, x19, [x0, #144]
    stp     x20, x21, [x0, #160]
    stp     x22, x23, [x0, #176]
    stp     x24, x25, [x0, #192]
    stp     x26, x27, [x0, #208]
    stp     x28, x29, [x0, #224]
    str     x30, [x0, #240]
    ldp     x2, x3, [sp], #16
    stp     x2, x3, [x0, #0]

    mrs     x2, esr_el2
    mrs     x3, far_el2
    mrs     x4, hpfar_el2
    str     x2, [x0, #HYP_OFF_ESR]
    str     x3, [x0, #HYP_OFF_FAR]
    str     x4, [x0, #HYP_OFF_HPFAR]
    mrs     x2, elr_el2
    mrs     x3, spsr_el2
    str     x2, [x0, #HYP_OFF_PC]
    str     x3, [x0, #HYP_OFF_PSTATE]

    add     x2, x0, #HYP_OFF_SYS
    save_el1 x2

    /* Its timer kept and switched off: the host does not use the virtual
     * timer, and the guest's must not fire while the guest is not there. */
    mrs     x3, cntv_ctl_el0
    mrs     x4, cntv_cval_el0
    str     x3, [x0, #HYP_OFF_CNTV_CTL]
    str     x4, [x0, #HYP_OFF_CNTV_CVAL]
    msr     cntv_ctl_el0, xzr

    /* Its interrupts kept - an ended one reads back as free - and the
     * virtual interface off. */
    adrp    x3, hyp_gic
    ldrb    w3, [x3, :lo12:hyp_gic]
    cbz     w3, 3f
    add     x5, x0, #HYP_OFF_LR
    mrs     x3, ICH_LR0_EL2
    mrs     x4, ICH_LR1_EL2
    stp     x3, x4, [x5]
    mrs     x3, ICH_LR2_EL2
    mrs     x4, ICH_LR3_EL2
    stp     x3, x4, [x5, #16]
    mrs     x3, ICH_VMCR_EL2
    str     x3, [x0, #HYP_OFF_VMCR]
    msr     ICH_HCR_EL2, xzr
3:
    add     x2, x0, #HYP_OFF_HOST_SYS
    load_el1 x2

    movz    x2, #0x8000, lsl #16        /* HCR_HOST */
    msr     hcr_el2, x2
    msr     vttbr_el2, xzr
    mov     x2, #CPTR_HOST
    msr     cptr_el2, x2
    mov     x2, #CNTHCTL_HOST
    msr     cnthctl_el2, x2
    msr     tpidr_el2, xzr

    add     x2, x0, #HYP_OFF_HOST_ELR
    ldp     x3, x4, [x2]
    msr     elr_el2, x3
    msr     spsr_el2, x4

    add     x2, x0, #HYP_OFF_HOST_GP
    ldp     x19, x20, [x2, #0]
    ldp     x21, x22, [x2, #16]
    ldp     x23, x24, [x2, #32]
    ldp     x25, x26, [x2, #48]
    ldp     x27, x28, [x2, #64]
    ldp     x29, x30, [x2, #80]

    ldr     x0, [x0, #HYP_OFF_EXIT]
    isb
    eret

/*
 * Kept in .data rather than .bss: core 0 writes them before `boot/start.S`
 * zeroes .bss.
 */
.data
.global hyp_ready
hyp_ready:
    .fill   HYP_CORES, 1, 0
hyp_gic:
    .byte   0                           /* the core has GICv3 system registers */
.balign 8
hyp_stuck_esr:
    .quad   0

.bss
.balign 16
hyp_stacks:
    .skip   (HYP_CORES + 1) * HYP_STACK_BYTES
