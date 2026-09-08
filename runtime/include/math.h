/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef MATH_H
#define MATH_H

/*
 * Where the arithmetic comes from, and the line between the two halves.
 *
 * The functions defined in runtime/libc/math.c are pure IEEE-754 bit
 * manipulation: what a double looks like in memory, and how to round one
 * without leaving the integer registers. That is worth writing, it is
 * exactly the sort of thing this project exists to understand, and it is
 * about 150 lines.
 *
 * The rest are musl's, vendored in `runtime/upstream/musl-math/`. A correct
 * `pow` is numerical analysis, not operating systems, and writing a bad one
 * would be worse than not having it. `design.md` §17.4 already sanctions
 * exactly this category: computational libraries with little system
 * surface.
 *
 * **They used to come from newlib's libm.a, and that was a dependency
 * nobody had decided on.** It worked because ARM's official GNU toolchain
 * happens to bundle newlib beside the compiler; Homebrew's
 * `x86_64-elf-gcc` ships `libgcc.a` and nothing else, and there is no
 * `x86_64-elf-newlib` to install. So the second architecture found that
 * this system's userland had been linking a libc it does not use and does
 * not want - the same shape as `<inttypes.h>`, which is not a freestanding
 * header and was being taken from the same place.
 *
 * Vendoring makes both machines compute `sin` with the same code, which
 * matters for a system whose point is that the same Lua runs on both.
 */

#define HUGE_VAL    __builtin_huge_val()
#define INFINITY    __builtin_inff()
#define NAN         __builtin_nanf("")

/*
 * The evaluation types, which C99 requires a `math.h` to define and this
 * one did not until something asked.
 *
 * `FLT_EVAL_METHOD` is the *compiler's* to say, not ours - it is in
 * `<float.h>`, which is one of the headers a freestanding implementation
 * has to provide, so it is always there. Defining it here instead was the
 * first thing tried and it is a redefinition warning on every file that
 * includes both.
 *
 * It says how much precision the machine uses for intermediate results.
 * Both of this system's targets answer 0, meaning each type is evaluated in
 * its own precision: AArch64 has no wider format in its registers, and
 * x86-64 evaluates in SSE rather than on the x87's 80-bit stack. A 32-bit
 * x86 would answer 2 and every `double_t` would be a `long double`; Kosmos
 * has decided it will not have one, and the `#error` says so rather than
 * silently getting it wrong.
 */
#include <float.h>

#if FLT_EVAL_METHOD == 0
typedef float  float_t;
typedef double double_t;
#elif FLT_EVAL_METHOD == 1
typedef double float_t;
typedef double double_t;
#else
#error "this machine evaluates floats wider than Kosmos expects"
#endif

/*
 * What `ilogb` answers for the two arguments that have no exponent. The
 * values are the ones every implementation uses and are what vendored code
 * compares against.
 */
#define FP_ILOGB0   (-2147483647 - 1)
#define FP_ILOGBNAN (-2147483647 - 1)

/* The classes `fpclassify` names, in the order that is conventional rather
 * than meaningful: nothing depends on the numbers, only on their being
 * distinct. */
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

/*
 * The classification macros, from the compiler rather than from a function
 * call.
 *
 * GCC turns every one of these into the two or three instructions that
 * actually answer the question - a comparison, or a test of the exponent
 * field - so `isnan(x)` costs nothing and needs no library. They have to be
 * macros rather than functions because they are type-generic: the same
 * spelling has to work on a float, a double and a long double, which C had
 * no way to express when they were standardised and only gained with
 * `_Generic` twelve years later.
 */
#define fpclassify(x)   __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, \
                                             FP_SUBNORMAL, FP_ZERO, (x))
#define isnan(x)        __builtin_isnan(x)
#define isinf(x)        __builtin_isinf(x)
#define isfinite(x)     __builtin_isfinite(x)
#define isnormal(x)     __builtin_isnormal(x)
#define signbit(x)      __builtin_signbit(x)

#define isgreater(x, y)      __builtin_isgreater(x, y)
#define isgreaterequal(x, y) __builtin_isgreaterequal(x, y)
#define isless(x, y)         __builtin_isless(x, y)
#define islessequal(x, y)    __builtin_islessequal(x, y)
#define islessgreater(x, y)  __builtin_islessgreater(x, y)
#define isunordered(x, y)    __builtin_isunordered(x, y)

/*
 * The constants, which are not standard C at all.
 *
 * `M_PI` and its family are POSIX rather than ISO, and are here because
 * vendored code uses them and every real `math.h` has them. Written to
 * more digits than a double can hold, which is deliberate: the compiler
 * rounds once, correctly, and a constant written to exactly seventeen
 * digits is one somebody will eventually shorten.
 */
#define M_E         2.7182818284590452354
#define M_LOG2E     1.4426950408889634074
#define M_LOG10E    0.43429448190325182765
#define M_LN2       0.69314718055994530942
#define M_LN10      2.30258509299404568402
#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.78539816339744830962
#define M_1_PI      0.31830988618379067154
#define M_2_PI      0.63661977236758134308
#define M_2_SQRTPI  1.12837916709551257390
#define M_SQRT2     1.41421356237309504880
#define M_SQRT1_2   0.70710678118654752440

/* Ours. runtime/libc/math.c. */
double fabs(double x);
double trunc(double x);
double floor(double x);
double ceil(double x);
double frexp(double x, int *exponent);
double ldexp(double x, int exponent);

/* newlib's. */
double fmod(double x, double y);
double pow(double x, double y);
double sqrt(double x);
double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

/*
 * The ones the vendored sources call on each other. Declared here rather
 * than only inside musl's own headers, because `runtime/include/math.h` is
 * what this system's own code compiles against and a function that exists
 * in the archive and not in the header is one nobody can reach.
 */
double scalbn(double x, int n);
float  scalbnf(float x, int n);
double copysign(double x, double y);
float  copysignf(float x, float y);
double fabs(double x);
float  fabsf(float x);
float  sqrtf(float x);
float  floorf(float x);
double round(double x);
double rint(double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double expm1(double x);
double log1p(double x);
double cbrt(double x);
double hypot(double x, double y);
double fmin(double x, double y);
double fmax(double x, double y);

#endif /* MATH_H */
