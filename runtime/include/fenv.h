/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef FENV_H
#define FENV_H

#include <sys/cdefs.h>
__BEGIN_DECLS

/*
 * **The floating-point environment's names** (`docs/webkit.md` W1c): the
 * rounding modes and exception flags, each architecture's own bits, which
 * LLVM's shared number parsing - C++'s `from_chars` - reads to know how a
 * result is rounded.
 *
 * The functions that would change the environment are not declared: the
 * mode is the one every process is born with, round to nearest, and
 * nothing here changes it. A port that calls `fesetround` fails at the
 * link and says so.
 */
#if defined(__x86_64__)
/* MXCSR's and the x87 status word's bits (Intel SDM vol. 1, 8.1.3, 10.2.3). */
#define FE_INVALID      0x01
#define FE_DIVBYZERO    0x04
#define FE_OVERFLOW     0x08
#define FE_UNDERFLOW    0x10
#define FE_INEXACT      0x20
#define FE_TONEAREST    0x000
#define FE_DOWNWARD     0x400
#define FE_UPWARD       0x800
#define FE_TOWARDZERO   0xc00

typedef unsigned short fexcept_t;
typedef struct {
    unsigned char x87[28];      /* fnstenv's image */
    unsigned int  mxcsr;
} fenv_t;
#elif defined(__aarch64__)
/* FPSR's cumulative flags and FPCR.RMode (Arm ARM, C5.2.8, C5.2.9). */
#define FE_INVALID      0x01
#define FE_DIVBYZERO    0x02
#define FE_OVERFLOW     0x04
#define FE_UNDERFLOW    0x08
#define FE_INEXACT      0x10
#define FE_TONEAREST    0x000000
#define FE_UPWARD       0x400000
#define FE_DOWNWARD     0x800000
#define FE_TOWARDZERO   0xc00000

typedef unsigned int fexcept_t;
typedef struct {
    unsigned int fpcr;
    unsigned int fpsr;
} fenv_t;
#else
#error "fenv.h: which architecture is this?"
#endif

#define FE_ALL_EXCEPT   (FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT)

__END_DECLS

#endif /* FENV_H */
