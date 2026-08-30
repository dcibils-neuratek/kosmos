/*
 * snprintf, enough of it.
 *
 * Lua turns every number into text through this, so it is on the path of
 * anything the REPL prints. It supports what Lua asks for and refuses the
 * rest loudly rather than printing something plausible and wrong:
 *
 *   %d %i %u %x %X %o %c %s %p %%
 *   length modifiers l, ll, z
 *   %f %e %g and their capitals
 *   width, left alignment, zero padding, precision
 *
 * The float conversion is NOT correctly rounded. It normalises by repeated
 * multiplication and extracts digits one at a time, so the last of the
 * seventeen digits a double can carry may be off by one. That is a real
 * limitation and it is written down rather than hidden: Lua's default format
 * is %.14g, three digits short of where it starts to show, and the honest
 * fix is a Grisu or Ryu implementation, which is a project of its own and
 * buys nothing until something needs exact round-tripping.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <panic.h>

/* Where the output goes. Tracking `written` past the end is what lets
 * snprintf return the length it would have needed, which callers use to
 * size a buffer. */
struct out {
    char  *buf;
    size_t cap;
    size_t written;
};

static void emit(struct out *o, char c)
{
    if (o->written + 1 < o->cap) {
        o->buf[o->written] = c;
    }
    o->written++;
}

static void emit_str(struct out *o, const char *s, size_t n)
{
    while (n-- > 0) {
        emit(o, *s++);
    }
}

struct spec {
    bool left;
    bool zero;
    bool plus;
    bool space;
    bool alt;
    int  width;
    int  precision;     /* -1 when absent */
};

static void pad(struct out *o, const struct spec *sp, size_t len, char with)
{
    int n = sp->width - (int)len;

    while (n-- > 0) {
        emit(o, with);
    }
}

/* Digits come out backwards, so every conversion fills a buffer from the end
 * and this reverses it into the output with the padding applied. */
static void emit_padded(struct out *o, const struct spec *sp,
                        const char *prefix, const char *digits, size_t ndigits)
{
    size_t plen = 0;
    size_t total;

    while (prefix[plen] != '\0') {
        plen++;
    }

    total = plen + ndigits;

    if (!sp->left && !sp->zero) {
        pad(o, sp, total, ' ');
    }

    emit_str(o, prefix, plen);

    if (!sp->left && sp->zero) {
        pad(o, sp, total, '0');
    }

    emit_str(o, digits, ndigits);

    if (sp->left) {
        pad(o, sp, total, ' ');
    }
}

static void format_unsigned(struct out *o, const struct spec *sp,
                            uint64_t value, unsigned base, bool upper,
                            const char *prefix)
{
    static const char lower_digits[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";
    const char *set = upper ? upper_digits : lower_digits;

    /* 64 bits in base 2 would be 64 characters; base 8 is the widest this
     * actually produces, at 22. */
    char tmp[24];
    size_t n = 0;

    if (value == 0) {
        tmp[n++] = '0';
    }

    while (value > 0) {
        tmp[n++] = set[value % base];
        value /= base;
    }

    /* Reverse in place; emit_padded wants them in reading order. */
    for (size_t i = 0; i < n / 2; i++) {
        char c = tmp[i];
        tmp[i] = tmp[n - 1 - i];
        tmp[n - 1 - i] = c;
    }

    emit_padded(o, sp, prefix, tmp, n);
}

static void format_signed(struct out *o, const struct spec *sp, int64_t value)
{
    const char *prefix = "";
    uint64_t magnitude;

    if (value < 0) {
        prefix = "-";
        /* Negating INT64_MIN overflows. Going through unsigned does not, and
         * the two's complement result is the magnitude we want. */
        magnitude = (uint64_t)(-(value + 1)) + 1;
    } else {
        magnitude = (uint64_t)value;
        if (sp->plus) {
            prefix = "+";
        } else if (sp->space) {
            prefix = " ";
        }
    }

    format_unsigned(o, sp, magnitude, 10, false, prefix);
}

/*
 * Floating point.
 *
 * Splits the value into a decimal exponent and a run of significant digits,
 * then lays them out in either fixed or exponential form. The normalisation
 * loop is what costs the precision: each multiplication by ten rounds, and
 * seventeen of them accumulate.
 */
#define MAX_DIGITS  18

struct decimal {
    char digits[MAX_DIGITS + 2];    /* room for a carry out of the top */
    int  ndigits;
    int  exponent;                  /* value = 0.digits * 10^exponent */
    bool negative;
};

static void decompose(double v, int significant, struct decimal *out)
{
    int exponent = 0;
    int i;

    out->negative = false;

    if (v < 0.0) {
        out->negative = true;
        v = -v;
    }

    if (v != 0.0) {
        /* Coarse steps first so a value near the limits of the format does
         * not take three hundred trips round the fine loop. */
        while (v >= 1e32) { v /= 1e32; exponent += 32; }
        while (v < 1e-32) { v *= 1e32; exponent -= 32; }
        while (v >= 1.0)  { v /= 10.0; exponent += 1; }
        while (v < 0.1)   { v *= 10.0; exponent -= 1; }
    }

    /* v is now in [0.1, 1). Peel digits off the top. */
    for (i = 0; i < significant; i++) {
        v *= 10.0;
        int d = (int)v;
        if (d > 9) { d = 9; }
        out->digits[i] = (char)('0' + d);
        v -= (double)d;
    }

    /* Round on the first digit that did not fit. */
    if (v >= 0.5) {
        int j = significant - 1;
        while (j >= 0) {
            if (out->digits[j] != '9') {
                out->digits[j]++;
                break;
            }
            out->digits[j] = '0';
            j--;
        }
        if (j < 0) {
            /* Every digit carried: 999 became 000 and there is a 1 above. */
            for (j = significant; j > 0; j--) {
                out->digits[j] = out->digits[j - 1];
            }
            out->digits[0] = '1';
            significant++;
            exponent++;
        }
    }

    out->ndigits = significant;
    out->exponent = exponent;
}

static void emit_fixed(struct out *o, const struct decimal *d, int frac,
                       bool strip)
{
    char tmp[64];
    size_t n = 0;
    int i;
    int digit_index = 0;

    if (d->exponent <= 0) {
        tmp[n++] = '0';
    } else {
        for (i = 0; i < d->exponent; i++) {
            tmp[n++] = (digit_index < d->ndigits)
                     ? d->digits[digit_index++] : '0';
        }
    }

    if (frac > 0) {
        size_t point = n;
        tmp[n++] = '.';

        for (i = 0; i < frac; i++) {
            if (i < -d->exponent) {
                tmp[n++] = '0';
            } else {
                tmp[n++] = (digit_index < d->ndigits)
                         ? d->digits[digit_index++] : '0';
            }
        }

        if (strip) {
            while (n > point + 1 && tmp[n - 1] == '0') {
                n--;
            }
            if (n == point + 1) {
                n = point;      /* nothing left after the point */
            }
        }
    }

    emit_str(o, tmp, n);
}

static void format_double(struct out *o, const struct spec *sp, double v,
                          char conv)
{
    struct decimal d;
    const char *sign = "";
    int precision = (sp->precision < 0) ? 6 : sp->precision;
    bool upper = (conv >= 'A' && conv <= 'Z');
    char lower = upper ? (char)(conv + 32) : conv;

    /* Infinity and NaN never reach the digit machinery. */
    if (v != v) {
        emit_str(o, upper ? "NAN" : "nan", 3);
        return;
    }
    if (v > 1.7976931348623157e308 || v < -1.7976931348623157e308) {
        if (v < 0) { emit(o, '-'); }
        emit_str(o, upper ? "INF" : "inf", 3);
        return;
    }

    if (lower == 'g' && precision == 0) {
        precision = 1;
    }

    decompose(v, (lower == 'f') ? MAX_DIGITS : precision, &d);

    if (d.negative) {
        sign = "-";
    } else if (sp->plus) {
        sign = "+";
    } else if (sp->space) {
        sign = " ";
    }

    emit_str(o, sign, (sign[0] == '\0') ? 0 : 1);

    if (v == 0.0) {
        /* decompose leaves the exponent at zero, which the layouts below
         * would render as "0" plus whatever fraction was asked for. Doing it
         * here keeps them from having to care. */
        emit(o, '0');
        if (lower == 'f' && precision > 0) {
            emit(o, '.');
            for (int i = 0; i < precision; i++) { emit(o, '0'); }
        }
        return;
    }

    if (lower == 'f') {
        emit_fixed(o, &d, precision, false);
        return;
    }

    /*
     * %g chooses between the two layouts the way the standard says:
     * exponential when the exponent is below -4 or at least the precision.
     * The exponent it tests is the one in scientific notation, which is one
     * less than the one decompose produces.
     */
    if (lower == 'g') {
        int sci = d.exponent - 1;

        if (sci < -4 || sci >= precision) {
            lower = 'e';
        } else {
            emit_fixed(o, &d, precision - d.exponent, true);
            return;
        }
    }

    /* Exponential. One digit, the point, then the rest. */
    {
        struct spec plain = { false, false, false, false, false, 0, -1 };
        int sci = d.exponent - 1;
        int frac = (lower == 'e') ? precision : d.ndigits - 1;

        emit(o, d.digits[0]);

        if (frac > 0) {
            size_t start = o->written;
            emit(o, '.');
            for (int i = 1; i <= frac; i++) {
                emit(o, (i < d.ndigits) ? d.digits[i] : '0');
            }
            if (conv == 'g' || conv == 'G') {
                /* %g strips trailing zeros; %e keeps them. Rewinding is safe
                 * because nothing has been written past here. */
                while (o->written > start + 1
                       && o->written - 1 < o->cap
                       && o->buf[o->written - 1] == '0') {
                    o->written--;
                }
                if (o->written == start + 1) {
                    o->written = start;
                }
            }
        }

        emit(o, upper ? 'E' : 'e');
        emit(o, (sci < 0) ? '-' : '+');
        plain.width = 2;
        plain.zero = true;
        format_unsigned(o, &plain, (uint64_t)((sci < 0) ? -sci : sci), 10,
                        false, "");
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };

    while (*fmt != '\0') {
        struct spec sp = { false, false, false, false, false, 0, -1 };
        int length = 0;             /* 0 = int, 1 = long, 2 = long long */

        if (*fmt != '%') {
            emit(&o, *fmt++);
            continue;
        }

        fmt++;

        for (;;) {
            if      (*fmt == '-') { sp.left = true; }
            else if (*fmt == '0') { sp.zero = true; }
            else if (*fmt == '+') { sp.plus = true; }
            else if (*fmt == ' ') { sp.space = true; }
            else if (*fmt == '#') { sp.alt = true; }
            else                  { break; }
            fmt++;
        }

        while (*fmt >= '0' && *fmt <= '9') {
            sp.width = sp.width * 10 + (*fmt++ - '0');
        }

        if (*fmt == '.') {
            fmt++;
            sp.precision = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                sp.precision = sp.precision * 10 + (*fmt++ - '0');
            }
        }

        while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h' || *fmt == 'j') {
            if (*fmt == 'l' || *fmt == 'z' || *fmt == 'j') {
                length++;
            }
            fmt++;
        }

        switch (*fmt) {
        case 'd':
        case 'i':
            format_signed(&o, &sp,
                          (length > 0) ? va_arg(ap, int64_t)
                                       : (int64_t)va_arg(ap, int));
            break;

        case 'u':
            format_unsigned(&o, &sp,
                            (length > 0) ? va_arg(ap, uint64_t)
                                         : (uint64_t)va_arg(ap, unsigned),
                            10, false, "");
            break;

        case 'x':
        case 'X':
            format_unsigned(&o, &sp,
                            (length > 0) ? va_arg(ap, uint64_t)
                                         : (uint64_t)va_arg(ap, unsigned),
                            16, *fmt == 'X', sp.alt ? "0x" : "");
            break;

        case 'o':
            format_unsigned(&o, &sp,
                            (length > 0) ? va_arg(ap, uint64_t)
                                         : (uint64_t)va_arg(ap, unsigned),
                            8, false, "");
            break;

        case 'c':
            emit(&o, (char)va_arg(ap, int));
            break;

        case 's': {
            const char *s = va_arg(ap, const char *);
            size_t n = 0;

            if (s == NULL) {
                s = "(null)";
            }

            while (s[n] != '\0'
                   && (sp.precision < 0 || n < (size_t)sp.precision)) {
                n++;
            }

            if (!sp.left) { pad(&o, &sp, n, ' '); }
            emit_str(&o, s, n);
            if (sp.left)  { pad(&o, &sp, n, ' '); }
            break;
        }

        case 'p': {
            struct spec plain = { false, true, false, false, false, 16, -1 };
            format_unsigned(&o, &plain,
                            (uint64_t)(uintptr_t)va_arg(ap, void *),
                            16, false, "0x");
            break;
        }

        case 'f': case 'F':
        case 'e': case 'E':
        case 'g': case 'G':
            format_double(&o, &sp, va_arg(ap, double), *fmt);
            break;

        case '%':
            emit(&o, '%');
            break;

        default:
            /*
             * An unsupported conversion. Printing it literally would produce
             * output that looks almost right and is silently wrong, and the
             * argument has already been left unconsumed, so everything after
             * it would be garbage too.
             */
            panic("vsnprintf: unsupported conversion specifier");
        }

        fmt++;
    }

    if (o.cap > 0) {
        o.buf[(o.written < o.cap) ? o.written : o.cap - 1] = '\0';
    }

    return (int)o.written;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);

    return n;
}
