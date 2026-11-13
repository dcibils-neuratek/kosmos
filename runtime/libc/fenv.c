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

/*
 * **And its exception flags** (`docs/w5.md` W5e, for musl's `fmaf`): the
 * same registers' sticky bits, whose positions the FE_ names already are -
 * MXCSR bits 0-5 and the x87 status word's bits 0-5 on x86-64 (Intel SDM
 * vol. 1, 10.2.3.1 and 8.1.3), FPSR bits 0-4 on AArch64 (Arm ARM, C5.2.9).
 * Raising one sets its bit; nothing traps, because every exception is
 * masked, as a process starts.
 */

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

/* The x87 environment's status word, at byte 4 of `fnstenv`'s image. */
static void x87_flags(int clear)
{
    unsigned char env[28];

    __asm__ volatile("fnstenv %0" : "=m"(env));
    env[4] = (unsigned char)(env[4] & ~clear);
    __asm__ volatile("fldenv %0" : : "m"(env));
}

int feclearexcept(int excepts)
{
    unsigned int mxcsr;

    excepts &= FE_ALL_EXCEPT;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    mxcsr &= ~(unsigned int)excepts;
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    x87_flags(excepts);
    return 0;
}

int fetestexcept(int excepts)
{
    unsigned int mxcsr;
    unsigned short status;

    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    __asm__ volatile("fnstsw %0" : "=m"(status));
    return (int)((mxcsr | status) & (unsigned int)excepts & FE_ALL_EXCEPT);
}

int feraiseexcept(int excepts)
{
    unsigned int mxcsr;

    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    mxcsr |= (unsigned int)(excepts & FE_ALL_EXCEPT);
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

int feclearexcept(int excepts)
{
    unsigned long fpsr;

    __asm__ volatile("mrs %0, fpsr" : "=r"(fpsr));
    fpsr &= ~(unsigned long)(excepts & FE_ALL_EXCEPT);
    __asm__ volatile("msr fpsr, %0" : : "r"(fpsr));
    return 0;
}

int fetestexcept(int excepts)
{
    unsigned long fpsr;

    __asm__ volatile("mrs %0, fpsr" : "=r"(fpsr));
    return (int)(fpsr & (unsigned long)(excepts & FE_ALL_EXCEPT));
}

int feraiseexcept(int excepts)
{
    unsigned long fpsr;

    __asm__ volatile("mrs %0, fpsr" : "=r"(fpsr));
    fpsr |= (unsigned long)(excepts & FE_ALL_EXCEPT);
    __asm__ volatile("msr fpsr, %0" : : "r"(fpsr));
    return 0;
}

#else
#error "fenv.c: which architecture is this?"
#endif
