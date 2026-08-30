#ifndef ARCH_AARCH64_TRAP_H
#define ARCH_AARCH64_TRAP_H

/*
 * The exception path.
 *
 * This header is included from both C and assembly, so everything above the
 * __ASSEMBLER__ guard has to be plain preprocessor. The offsets below are
 * what vectors.S uses to build the frame; the _Static_asserts at the bottom
 * are what keeps the two from drifting apart, which is a class of bug that
 * corrupts registers and surfaces five functions later.
 */

/* Byte offsets into struct trapframe. x0..x30 are a plain array at 0. */
#define TF_X0       0
#define TF_SP       248     /* sp as it was before the exception */
#define TF_ELR      256
#define TF_SPSR     264
#define TF_ESR      272
#define TF_FAR      280
#define TF_SIZE     288     /* 16-byte aligned, as the ABI requires of sp */

#ifndef __ASSEMBLER__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct trapframe {
    uint64_t x[31];     /* x0 through x30 */
    uint64_t sp;
    uint64_t elr;       /* the instruction that faulted */
    uint64_t spsr;
    uint64_t esr;       /* what kind of exception */
    uint64_t far;       /* the address it touched, for aborts */
};

_Static_assert(sizeof(struct trapframe) == TF_SIZE,
               "trapframe size disagrees with vectors.S");
_Static_assert(offsetof(struct trapframe, x)    == TF_X0,   "TF_X0");
_Static_assert(offsetof(struct trapframe, sp)   == TF_SP,   "TF_SP");
_Static_assert(offsetof(struct trapframe, elr)  == TF_ELR,  "TF_ELR");
_Static_assert(offsetof(struct trapframe, spsr) == TF_SPSR, "TF_SPSR");
_Static_assert(offsetof(struct trapframe, esr)  == TF_ESR,  "TF_ESR");
_Static_assert(offsetof(struct trapframe, far)  == TF_FAR,  "TF_FAR");

/* ESR_EL1 field extraction. ARM ARM D17.2.37. */
#define ESR_EC(esr)     (((esr) >> 26) & 0x3f)
#define ESR_ISS(esr)    ((esr) & 0x1ffffff)

#define EC_UNKNOWN          0x00
#define EC_SVC64            0x15
#define EC_IABT_LOWER       0x20
#define EC_IABT_SAME        0x21
#define EC_PC_ALIGN         0x22
#define EC_DABT_LOWER       0x24
#define EC_DABT_SAME        0x25
#define EC_SP_ALIGN         0x26
#define EC_BRK64            0x3c

/* Data abort ISS: bit 6 says which direction, bits 5:0 are the fault code. */
#define ISS_DABT_WNR(iss)   (((iss) >> 6) & 1)
#define ISS_DABT_DFSC(iss)  ((iss) & 0x3f)

/* Installs the vector table in VBAR_EL1. Call before anything can fault. */
void trap_init(void);

/*
 * Deliberate faults, for tests.
 *
 * A kernel test that checks something fails correctly needs the handler to
 * treat one exception as expected: record it and carry on, instead of
 * reporting a panic and halting. `testing.md` §18.2 calls for this at M1,
 * because retrofitting it into a vector that is already written is worse.
 *
 * Recovery is by stepping ELR past the faulting instruction rather than by
 * longjmp: there is no setjmp until the libc arrives at M2, and every A64
 * instruction is exactly four bytes, so the arithmetic is exact.
 *
 *     fault_expect_begin();
 *     *(volatile int *)0 = 1;
 *     if (!fault_expect_end(&info)) { ... it did not fault ... }
 *
 * Not reentrant, and not meant to be: one armed fault at a time.
 */
struct fault_info {
    uint64_t esr;
    uint64_t far;
    uint64_t elr;
};

void fault_expect_begin(void);

/* Disarms, and returns whether a fault actually fired. Fills *out when it
 * did; *out is untouched otherwise. */
bool fault_expect_end(struct fault_info *out);

#endif /* !__ASSEMBLER__ */

#endif /* ARCH_AARCH64_TRAP_H */
