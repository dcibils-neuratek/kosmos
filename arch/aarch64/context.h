#ifndef ARCH_AARCH64_CONTEXT_H
#define ARCH_AARCH64_CONTEXT_H

/*
 * What a switched-out thread consists of.
 *
 * Included from both C and assembly, so everything above the __ASSEMBLER__
 * guard is plain preprocessor and the _Static_asserts below are what keep the
 * offsets from drifting away from switch.S. Getting one of them wrong
 * corrupts a register in a way that surfaces five functions later, which is
 * the trap roadmap.md warns about for this milestone.
 */

#define CTX_X19     0
#define CTX_X21     16
#define CTX_X23     32
#define CTX_X25     48
#define CTX_X27     64
#define CTX_X29     80      /* x29, x30 */
#define CTX_SP      96      /* sp_el0, sp_el1 */
#define CTX_DAIF    112
#define CTX_SPSEL   120
/*
 * The whole FP and SIMD register file, not the callee-saved half.
 *
 * The half is the right answer for a thread that *called* this: the
 * compiler already spilled anything else it cared about. A preempted thread
 * called nothing. It was stopped between two instructions with whatever was
 * live still in v0 to v7 and v16 to v31, and the exception entry saves no FP
 * state at all - so the thread scheduled next overwrites them and the first
 * one resumes mid-expression with the second one's numbers.
 *
 * That is a wrong answer and never a crash, which is why it survived this
 * long: nothing in Lua's own path holds a double in a register across more
 * than one VM opcode, so the window is a few instructions wide and the
 * corruption is rare and silent. `test_fp_survives_a_preemption` catches it
 * in about two spins in sixty.
 *
 * Saved here rather than on the exception path because here it is both
 * correct and much rarer: the kernel's own C is built -mgeneral-regs-only
 * and cannot touch an FP register, so by the time this runs the values are
 * still exactly what the interrupted thread left, and a switch is far less
 * frequent than an exception.
 *
 * Full 128-bit Q registers, not the low halves. Lua's numbers are doubles,
 * but the userland it is compiled into is not built -mgeneral-regs-only and
 * the compiler is free to use the upper halves; saving only what Lua's
 * arithmetic obviously needs would be the same bug one layer down.
 */
#define CTX_D8      128
#define CTX_D10     144
#define CTX_D12     160
#define CTX_D14     176
#define CTX_Q0      192     /* q0 through q31, sixteen pairs, 512 bytes */
#define CTX_FPCR    704     /* fpcr, fpsr */
#define CTX_SIZE    720

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>

struct context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t x29;       /* frame pointer */
    uint64_t x30;       /* where the thread resumes */
    uint64_t sp_el0;    /* the thread's own stack */
    uint64_t sp_el1;    /* the stack its exceptions are taken on */

    /*
     * The interrupt mask this thread was running under. Saved and restored
     * rather than assumed, so a switch is correct whether or not the caller
     * had interrupts enabled. Once IPC starts switching from places that
     * already hold them masked, assuming would be a source of interrupts
     * silently re-enabled halfway through a critical section.
     */
    uint64_t daif;

    /*
     * Which stack pointer this thread was using: 0 for SP_EL0, 1 for SP_EL1.
     *
     * It has to be saved because a switch can start from either. A thread
     * that yields is on SP_EL0; a thread taken off the CPU by the timer is
     * in the vector's epilogue, where the hardware set SPSel to 1 when it
     * took the exception. Assuming either one corrupts the other, and it
     * corrupts it by writing one stack pointer into both slots.
     */
    uint64_t spsel;

    uint64_t d8, d9, d10, d11, d12, d13, d14, d15;

    /*
     * v0 to v31, two 64-bit words each, and then the two control registers.
     * Aligned to 16 because `stp q` addresses it and an unaligned pair
     * access is a fault the moment SCTLR.A is ever set.
     */
    uint64_t v[64] __attribute__((aligned(16)));
    uint64_t fpcr, fpsr;
};

_Static_assert(sizeof(struct context) == CTX_SIZE, "context size vs switch.S");
_Static_assert(offsetof(struct context, x19)    == CTX_X19, "CTX_X19");
_Static_assert(offsetof(struct context, x21)    == CTX_X21, "CTX_X21");
_Static_assert(offsetof(struct context, x23)    == CTX_X23, "CTX_X23");
_Static_assert(offsetof(struct context, x25)    == CTX_X25, "CTX_X25");
_Static_assert(offsetof(struct context, x27)    == CTX_X27, "CTX_X27");
_Static_assert(offsetof(struct context, x29)    == CTX_X29, "CTX_X29");
_Static_assert(offsetof(struct context, sp_el0) == CTX_SP,   "CTX_SP");
_Static_assert(offsetof(struct context, daif)   == CTX_DAIF,  "CTX_DAIF");
_Static_assert(offsetof(struct context, spsel)  == CTX_SPSEL, "CTX_SPSEL");
_Static_assert(offsetof(struct context, d8)     == CTX_D8,  "CTX_D8");
_Static_assert(offsetof(struct context, d10)    == CTX_D10, "CTX_D10");
_Static_assert(offsetof(struct context, d12)    == CTX_D12, "CTX_D12");
_Static_assert(offsetof(struct context, d14)    == CTX_D14, "CTX_D14");
_Static_assert(offsetof(struct context, v)      == CTX_Q0,   "CTX_Q0");
_Static_assert(offsetof(struct context, fpcr)   == CTX_FPCR, "CTX_FPCR");

/* Saves into prev, loads from next, and returns inside next. */
void context_switch(struct context *prev, struct context *next);

/* Where a hand-built context starts. Never called directly. */
void thread_entry(void);

#endif /* !__ASSEMBLER__ */

#endif /* ARCH_AARCH64_CONTEXT_H */
