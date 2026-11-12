/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A thread's rounding mode** (`fenv.h`): read and set in the registers
 * that hold it, which Nebula saves with the thread's others, so a mode set
 * here is this thread's alone.
 *
 * x86-64 has two: MXCSR.RC (bits 13-14) for SSE, which is all the
 * compiler's floating point uses, and the x87 control word's RC (bits
 * 10-11) for `long double`; both are set, as every libc does, and the
 * mode read is MXCSR's (Intel SDM vol. 1, 10.2.3 and 8.1.5). AArch64 has
 * FPCR.RMode, bits 22-23 (Arm ARM, C5.2.8). The FE_ values are those
 * fields in place, so the mode is a mask and a shift away.
 */

#include <fenv.h>

#define ROUNDING (FE_TONEAREST | FE_DOWNWARD | FE_UPWARD | FE_TOWARDZERO)

static int known(int mode)
{
    return mode == FE_TONEAREST || mode == FE_DOWNWARD
        || mode == FE_UPWARD || mode == FE_TOWARDZERO;
}

#if defined(__x86_64__)

int fegetround(void)
{
    unsigned int mxcsr;

    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    return (int)((mxcsr >> 3) & ROUNDING);          /* bits 13-14 to 10-11 */
}

int fesetround(int mode)
{
    unsigned int mxcsr;
    unsigned short x87;

    if (!known(mode)) {
        return 1;
    }

    __asm__ volatile("fnstcw %0" : "=m"(x87));
    x87 = (unsigned short)((x87 & ~ROUNDING) | mode);
    __asm__ volatile("fldcw %0" : : "m"(x87));

    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    mxcsr = (mxcsr & ~(ROUNDING << 3)) | ((unsigned int)mode << 3);
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    return 0;
}

#elif defined(__aarch64__)

int fegetround(void)
{
    unsigned long fpcr;

    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    return (int)(fpcr & ROUNDING);
}

int fesetround(int mode)
{
    unsigned long fpcr;

    if (!known(mode)) {
        return 1;
    }

    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr = (fpcr & ~(unsigned long)ROUNDING) | (unsigned long)mode;
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
    return 0;
}

#else
#error "fenv.c: which architecture is this?"
#endif
