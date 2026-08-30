/*
 * The half of the maths that is bit manipulation rather than numerics.
 *
 * A double is one sign bit, eleven exponent bits biased by 1023, and
 * fifty-two mantissa bits with an implied leading 1:
 *
 *   63 62        52 51                                              0
 *   +--+-----------+------------------------------------------------+
 *   |s |  exponent |                    mantissa                    |
 *   +--+-----------+------------------------------------------------+
 *
 * The unbiased exponent says where the binary point sits inside the
 * mantissa. Everything below follows from that one fact: rounding towards
 * zero is masking off the mantissa bits below the point, and there is no
 * arithmetic involved at all.
 *
 * Compiled without -mgeneral-regs-only, unlike the kernel. See the Makefile.
 */

#include <stdint.h>
#include <math.h>

#define EXP_BIAS        1023
#define MANTISSA_BITS   52
#define EXP_MASK        0x7ffULL
#define SIGN_BIT        (1ULL << 63)

/* Reading a double's bits through a union is the one form of type punning
 * C explicitly blesses; a pointer cast would be an aliasing violation, and
 * the build uses -fno-strict-aliasing but does not rely on it. */
typedef union {
    double   d;
    uint64_t u;
} dbits;

static inline int unbiased_exponent(uint64_t u)
{
    return (int)((u >> MANTISSA_BITS) & EXP_MASK) - EXP_BIAS;
}

double fabs(double x)
{
    dbits b;

    b.d = x;
    b.u &= ~SIGN_BIT;
    return b.d;
}

double trunc(double x)
{
    dbits b;
    int e;
    uint64_t fraction;

    b.d = x;
    e = unbiased_exponent(b.u);

    if (e < 0) {
        /* |x| < 1, so everything is fraction. The sign survives, which is
         * what gives trunc(-0.5) its -0.0. */
        b.u &= SIGN_BIT;
        return b.d;
    }

    if (e >= MANTISSA_BITS) {
        /* No fractional bits left to drop. Also the path infinities and NaNs
         * take, and returning them unchanged is correct for both. */
        return x;
    }

    /* The bits below the binary point are the fraction. Mask them off. */
    fraction = (1ULL << (MANTISSA_BITS - e)) - 1;
    b.u &= ~fraction;
    return b.d;
}

double floor(double x)
{
    double t = trunc(x);

    /* trunc rounds towards zero, floor towards minus infinity. They differ
     * only for negatives that had a fraction to lose. */
    if (t > x) {
        return t - 1.0;
    }

    return t;
}

double ceil(double x)
{
    double t = trunc(x);

    if (t < x) {
        return t + 1.0;
    }

    return t;
}

double frexp(double x, int *exponent)
{
    dbits b;
    int e;

    b.d = x;
    e = (int)((b.u >> MANTISSA_BITS) & EXP_MASK);

    if (e == 0) {
        if ((b.u & ~SIGN_BIT) == 0) {
            /* Zero, either sign. */
            *exponent = 0;
            return x;
        }

        /*
         * Subnormal. Its exponent field is zero and there is no implied
         * leading 1, so the field cannot be read directly. Scaling up by
         * 2^54 makes it normal, and the 54 comes back off the exponent.
         */
        b.d = x * 18014398509481984.0;      /* 2^54 */
        e = (int)((b.u >> MANTISSA_BITS) & EXP_MASK) - 54;
    } else if (e == (int)EXP_MASK) {
        /* Infinity or NaN. No meaningful decomposition; the standard leaves
         * the exponent unspecified, so it gets a defined value instead. */
        *exponent = 0;
        return x;
    }

    /*
     * frexp returns a mantissa in [0.5, 1). That is the exponent field set
     * to 1022, meaning 2^-1, with the mantissa bits untouched.
     */
    *exponent = e - (EXP_BIAS - 1);
    b.u = (b.u & ~(EXP_MASK << MANTISSA_BITS))
        | ((uint64_t)(EXP_BIAS - 1) << MANTISSA_BITS);
    return b.d;
}

double ldexp(double x, int exponent)
{
    dbits b;
    int e;

    b.d = x;
    e = (int)((b.u >> MANTISSA_BITS) & EXP_MASK);

    /* Zero, infinity and NaN all scale to themselves. */
    if (e == 0 && (b.u & ~SIGN_BIT) == 0) {
        return x;
    }
    if (e == (int)EXP_MASK) {
        return x;
    }

    /*
     * Multiplying by a power of two is the honest way to do this rather than
     * adding to the exponent field: it lets the hardware handle overflow to
     * infinity and underflow into the subnormals, both of which the field
     * arithmetic would get wrong. Split into steps so the intermediate
     * cannot overflow on its own.
     */
    while (exponent > 1000) {
        x *= 8.98846567431158e307;          /* 2^1023 */
        exponent -= 1023;
    }
    while (exponent < -1000) {
        x *= 1.1125369292536007e-308;       /* 2^-1022 */
        exponent += 1022;
    }

    b.u = ((uint64_t)(exponent + EXP_BIAS) & EXP_MASK) << MANTISSA_BITS;
    return x * b.d;
}
