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
 * The rest come from newlib's libm.a, which the ARM toolchain already ships.
 * A correct `pow` is numerical analysis, not operating systems, and writing
 * a bad one would be worse than not having it. `design.md` §17.4 already
 * sanctions exactly this category: computational libraries with little
 * system surface. libm's only demand on us is `__errno`, which the design
 * wanted per-process anyway.
 */

#define HUGE_VAL    __builtin_huge_val()
#define INFINITY    __builtin_inff()
#define NAN         __builtin_nanf("")

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

#endif /* MATH_H */
