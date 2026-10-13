/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef ARCH_AARCH64_HYP_H
#define ARCH_AARCH64_HYP_H

/*
 * **A virtual CPU, as the processor's EL2 sees it** - `docs/virtualization.md`
 * step V1.
 *
 * Nebula runs at EL1 and stays there. What it gains is a small piece of
 * code at EL2, installed at boot while the processor is still there
 * (`boot/start.S`, `hyp.S`), which does exactly one thing for it: given one
 * of these, it puts the host's EL1 state aside, loads the guest's, turns on
 * the second stage of translation and enters the guest - and when the guest
 * does something EL2 traps, it puts everything back and returns to the
 * kernel with why. Linux's split mode on processors without VHE is the same
 * shape, and so is seL4's.
 *
 * **The guest's EL1 registers are the processor's EL1 registers** while it
 * runs. That is why the list below exists twice: a guest kernel writes its
 * own SCTLR_EL1, TTBR0_EL1 and the rest, and every one of them is also one
 * Nebula depends on. **Its FP and SIMD registers** are not here: they are
 * in the hardware while it runs, lent by `fp_lend` around each entry
 * (`fp.c`, `vcpu.c`). **Its interrupts** go through the GIC's virtual CPU
 * interface - the list registers and ICH_VMCR, below - and **its timer** is
 * the core's virtual one, the guest's while it runs and switched off while
 * the host does.
 *
 * The offsets are written out because the assembly reads them; the static
 * assertions in `vcpu.c` hold the struct to them.
 */

/* What the kernel asks of EL2, in x0 of an `hvc #0`. */
#define HYP_PING            0           /* answers HYP_PING_ANSWER */
#define HYP_RUN             1           /* x1: a struct hyp_vcpu, physical */
#define HYP_PING_ANSWER     0x4b4f534d4f53ul    /* "KOSMOS" */
#define HYP_UNKNOWN         ((unsigned long)-1)

/* Why a guest stopped: what `hyp_run` returns. */
#define HYP_EXIT_SYNC       1           /* an exception: `esr` says which */
#define HYP_EXIT_IRQ        2           /* the host's interrupt arrived */
#define HYP_EXIT_FIQ        3
#define HYP_EXIT_SERROR     4

/*
 * **The GIC's list registers a vCPU uses**: an interrupt the monitor wants
 * the guest to take is written into one, and the processor's virtual CPU
 * interface delivers it - the guest acknowledges and ends it through its
 * ordinary ICC registers, which HCR.IMO turns into the virtual ones. Four is
 * what QEMU's GICv3 and most cores have (ICH_VTR_EL2.ListRegs + 1).
 */
#define HYP_LRS             4

/* The EL1 registers a guest and the host each have their own of. */
#define HYP_SYS_COUNT       24          /* 23 and one to keep 16-byte pairs */

#define HYP_OFF_GP          0           /* x0..x30, then a pad */
#define HYP_OFF_PC          256         /* the guest's ELR_EL2 */
#define HYP_OFF_PSTATE      264         /* the guest's SPSR_EL2 */
#define HYP_OFF_SYS         272         /* the guest's EL1 registers */
#define HYP_OFF_HOST_SYS    (HYP_OFF_SYS + 8 * HYP_SYS_COUNT)       /* 464 */
#define HYP_OFF_HOST_GP     (HYP_OFF_HOST_SYS + 8 * HYP_SYS_COUNT)  /* 656: x19..x30 */
#define HYP_OFF_HOST_ELR    (HYP_OFF_HOST_GP + 8 * 12)              /* 752 */
#define HYP_OFF_HOST_SPSR   (HYP_OFF_HOST_ELR + 8)
#define HYP_OFF_VTTBR       (HYP_OFF_HOST_SPSR + 8)                 /* 768 */
#define HYP_OFF_FLUSH       (HYP_OFF_VTTBR + 8)
#define HYP_OFF_EXIT        (HYP_OFF_FLUSH + 8)                     /* 784 */
#define HYP_OFF_ESR         (HYP_OFF_EXIT + 8)
#define HYP_OFF_FAR         (HYP_OFF_ESR + 8)
#define HYP_OFF_HPFAR       (HYP_OFF_FAR + 8)
#define HYP_OFF_LR          (HYP_OFF_HPFAR + 8)                     /* 816: four */
#define HYP_OFF_VMCR        (HYP_OFF_LR + 8 * HYP_LRS)              /* 848 */
#define HYP_OFF_CNTV_CTL    (HYP_OFF_VMCR + 8)
#define HYP_OFF_CNTV_CVAL   (HYP_OFF_CNTV_CTL + 8)
#define HYP_VCPU_BYTES      (HYP_OFF_CNTV_CVAL + 16)                /* 880, with a pad */

/* How many cores the stub keeps a stack and a flag for; `vcpu.c` holds it
 * to NR_CPUS, which the assembly cannot include. */
#define HYP_CORES           8

/* Each core's own stack at EL2, for the handful of words the stub keeps. */
#define HYP_STACK_BYTES     512

#ifndef __ASSEMBLER__

#include <stdbool.h>
#include <stdint.h>

struct hyp_vcpu {
    uint64_t gp[32];                    /* x0..x30 and a pad */
    uint64_t pc;
    uint64_t pstate;
    uint64_t sys[HYP_SYS_COUNT];        /* the guest's, in `hyp.S`'s order */
    uint64_t host_sys[HYP_SYS_COUNT];   /* the host's, while the guest runs */
    uint64_t host_gp[12];
    uint64_t host_elr, host_spsr;
    uint64_t vttbr;                     /* stage 2's table, and the VMID at 48 */
    uint64_t flush;                     /* nonzero: drop this VMID's TLB first */
    uint64_t exit, esr, far, hpfar;     /* why it last stopped */
    uint64_t lr[HYP_LRS];               /* ICH_LR0-3_EL2, in and out */
    uint64_t vmcr;                      /* ICH_VMCR_EL2: its priority mask, groups on */
    uint64_t cntv_ctl, cntv_cval;       /* its virtual timer */
    uint64_t pad;                       /* to the struct's 16-byte alignment */
} __attribute__((aligned(16)));

/* The index of each EL1 register in `sys[]`, as `hyp.S` saves them. */
enum {
    HYP_SCTLR, HYP_TTBR0, HYP_TTBR1, HYP_TCR, HYP_MAIR, HYP_AMAIR, HYP_VBAR,
    HYP_CONTEXTIDR, HYP_TPIDR_EL1, HYP_SP_EL1, HYP_ELR_EL1, HYP_SPSR_EL1,
    HYP_ESR_EL1, HYP_FAR_EL1, HYP_AFSR0, HYP_AFSR1, HYP_CPACR, HYP_CNTKCTL,
    HYP_SP_EL0, HYP_TPIDR_EL0, HYP_TPIDRRO_EL0, HYP_PAR, HYP_CSSELR,
};

/* Whether this machine started Nebula at EL2, so there is a stub to ask. */
bool hyp_present(void);

/* An `hvc` to the stub: `HYP_PING` answers `HYP_PING_ANSWER`. */
unsigned long hyp_call(unsigned long function);

/* Runs the guest until it stops; answers a `HYP_EXIT_*`. */
unsigned long hyp_run(struct hyp_vcpu *vcpu);

/* Called by `boot/start.S` at EL2 on each core, before it leaves. */
void hyp_install(unsigned long core);

/* A guest's memory: a stage-2 root, a page mapped into it, the tables
 * freed (not the pages they map), and code made visible to a guest. */
uint64_t *stage2_new(void);
bool stage2_map(uint64_t *root, uint64_t ipa, uintptr_t pa);
void stage2_free(uint64_t *root);
void stage2_code_written(const void *start, unsigned long bytes);

#endif /* !__ASSEMBLER__ */

#endif /* ARCH_AARCH64_HYP_H */
