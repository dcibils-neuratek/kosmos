/*
 * strtod: text to double.
 *
 * The other half of the number path. snprintf turns a double into digits;
 * this turns digits back into a double, and Lua calls it for every float
 * literal it parses.
 *
 * Accuracy, stated rather than implied. Significant digits are accumulated
 * into a 64-bit integer, which is exact for the first nineteen, and the
 * decimal exponent is then applied in one multiply or divide. Powers of ten
 * up to 10^22 are exactly representable as doubles, so within that range the
 * result is off by at most one rounding. Outside it the scaling itself
 * rounds, and the error can reach a few units in the last place.
 *
 * That is the same trade snprintf makes in the other direction, for the same
 * reason: correct rounding needs arbitrary-precision arithmetic, and neither
 * Lua's %.14g output nor a hand-typed literal comes close to needing it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

/*
 * 10^0 through 10^22. Every one is exactly representable: a double has 53
 * bits of mantissa, and 10^22 is the largest power of ten that fits. 10^23
 * is not, which is where the exactness stops.
 */
static const double power_of_ten[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
    1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
    1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

#define EXACT_POWERS ((int)(sizeof(power_of_ten) / sizeof(power_of_ten[0])))

static double scale_by_ten(double value, int exponent)
{
    bool negative = exponent < 0;
    int e = negative ? -exponent : exponent;

    if (value == 0.0) {
        return value;
    }

    while (e > 0) {
        int step = (e < EXACT_POWERS) ? e : EXACT_POWERS - 1;

        /* One multiply per step, each by an exactly representable power, so
         * the rounding happens once per step rather than once per digit. */
        if (negative) {
            value /= power_of_ten[step];
        } else {
            value *= power_of_ten[step];
        }

        e -= step;
    }

    return value;
}

/* 0x1.8p3 and friends. Exact, because every hex digit is four bits of
 * mantissa and the exponent is applied with ldexp, which does not round. */
static double parse_hex(const char *s, const char **end)
{
    double value = 0.0;
    int exponent = 0;
    bool any = false;

    while (isxdigit((unsigned char)*s)) {
        int d = isdigit((unsigned char)*s)
              ? *s - '0'
              : (int)(tolower((unsigned char)*s) - 'a' + 10);
        value = value * 16.0 + (double)d;
        any = true;
        s++;
    }

    if (*s == '.') {
        s++;
        while (isxdigit((unsigned char)*s)) {
            int d = isdigit((unsigned char)*s)
                  ? *s - '0'
                  : (int)(tolower((unsigned char)*s) - 'a' + 10);
            value = value * 16.0 + (double)d;
            exponent -= 4;
            any = true;
            s++;
        }
    }

    if (!any) {
        return 0.0;     /* "0x" with nothing after it is not a number */
    }

    if (*s == 'p' || *s == 'P') {
        const char *after = s + 1;
        bool negative = false;
        int p = 0;

        if (*after == '+' || *after == '-') {
            negative = (*after == '-');
            after++;
        }

        if (isdigit((unsigned char)*after)) {
            while (isdigit((unsigned char)*after)) {
                p = p * 10 + (*after - '0');
                after++;
            }
            exponent += negative ? -p : p;
            s = after;
        }
        /* A 'p' with no digits is not part of the number; leave s before it. */
    }

    *end = s;
    return ldexp(value, exponent);
}

double strtod(const char *s, char **end)
{
    const char *start = s;
    const char *last_valid = s;
    bool negative = false;
    uint64_t mantissa = 0;
    int significant = 0;
    int exponent = 0;
    bool any_digits = false;
    double value;

    while (isspace((unsigned char)*s)) {
        s++;
    }

    if (*s == '+' || *s == '-') {
        negative = (*s == '-');
        s++;
    }

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        const char *after = s + 2;
        const char *hex_end = after;
        double hex = parse_hex(after, &hex_end);

        if (hex_end == after) {
            /* Not a hex float after all. "0x" parses as the 0 before it. */
            if (end != NULL) {
                *end = (char *)(uintptr_t)(s + 1);
            }
            return negative ? -0.0 : 0.0;
        }

        if (end != NULL) {
            *end = (char *)(uintptr_t)hex_end;
        }
        return negative ? -hex : hex;
    }

    while (isdigit((unsigned char)*s)) {
        any_digits = true;

        if (significant < 19) {
            mantissa = mantissa * 10 + (uint64_t)(*s - '0');
            if (mantissa != 0) {
                significant++;
            }
        } else {
            /* Past what a 64-bit integer holds exactly. The digit still
             * counts towards the magnitude, it just cannot be kept. */
            exponent++;
        }

        s++;
    }

    if (*s == '.') {
        s++;
        while (isdigit((unsigned char)*s)) {
            any_digits = true;

            if (significant < 19) {
                mantissa = mantissa * 10 + (uint64_t)(*s - '0');
                if (mantissa != 0) {
                    significant++;
                }
                exponent--;
            }

            s++;
        }
    }

    if (!any_digits) {
        /* Nothing was consumed, so nothing was a number. The standard wants
         * end left at the original string, not wherever the sign got to. */
        if (end != NULL) {
            *end = (char *)(uintptr_t)start;
        }
        return 0.0;
    }

    last_valid = s;

    if (*s == 'e' || *s == 'E') {
        const char *after = s + 1;
        bool exp_negative = false;
        int e = 0;

        if (*after == '+' || *after == '-') {
            exp_negative = (*after == '-');
            after++;
        }

        if (isdigit((unsigned char)*after)) {
            while (isdigit((unsigned char)*after)) {
                /* Clamped rather than allowed to overflow. Anything past
                 * this is already infinity or zero. */
                if (e < 100000) {
                    e = e * 10 + (*after - '0');
                }
                after++;
            }

            exponent += exp_negative ? -e : e;
            last_valid = after;
        }
        /* An 'e' with no digits after it is not part of the number. */
    }

    if (end != NULL) {
        *end = (char *)(uintptr_t)last_valid;
    }

    value = scale_by_ten((double)mantissa, exponent);

    return negative ? -value : value;
}
