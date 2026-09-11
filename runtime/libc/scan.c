/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The scanf family's one scanner, bounded by a length rather than a NUL.
 *
 * `sscanf` and `vsscanf` lived in `user/lib/misc_user.c`, next to the
 * `strtol` they call, and read until a NUL. That was enough for Doom, which
 * parses the lines of a config file. It is not enough for `fscanf`: a file
 * here is bytes in memory (`stdio.c`), and a file inside a pak - Quake's
 * demos are - is followed by the next file rather than by a NUL. So the
 * scanner takes a length and reports how much it consumed; `vsscanf` is that
 * with `strlen`, and `fscanf` is that over what is left of a `FILE`.
 *
 * Two things were wrong, and moving it is when they were found:
 *
 * - **`%f` stored a `double`.** The standard says `%f` takes a `float *` and
 *   `%lf` a `double *`. Doom only ever asks for `%lf`, so nothing noticed;
 *   Quake's `%f` into a `vec3_t` would have written four bytes past every
 *   value. Length modifiers are honoured now - none, and `l` - and any other
 *   stops the scan rather than writing the wrong width.
 * - **Numbers were parsed straight off the input** by `strtol` and `strtod`,
 *   which read until something is not part of a number: past the end, when
 *   the end is a length. A number's characters are copied into a small
 *   buffer first, as far as the bound, and parsed there.
 *
 * What it handles: whitespace, literal characters, `%%`, and
 * `%d %i %u %o %x %X %c %s %f %e %g` with an optional field width and `*`
 * suppression. Anything else stops the scan and returns what matched so far,
 * which is what the standard says to do, and loses a field rather than
 * misreading one.
 *
 * It includes nothing of Kosmos's, so `tools/test_scan.c` builds it on the
 * build machine.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int kosmos_vscan(const char *in, size_t len, const char *fmt, va_list ap,
                 size_t *used);

static int is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r'
        || c == '\v' || c == '\f';
}

/*
 * Could this character continue a number?
 *
 * Generous on purpose: `strtol` and `strtod` decide where the number really
 * ends, and the scan moves past what they used, not past what was copied.
 */
static int numberish(int c, int floating)
{
    if ((c >= '0' && c <= '9') || c == '+' || c == '-'
        || c == 'x' || c == 'X'
        || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
        return 1;
    }

    /* A decimal point, a binary exponent, and the letters of inf and nan. */
    return floating && (c == '.' || c == 'p' || c == 'P'
                        || c == 'i' || c == 'I' || c == 'n' || c == 'N'
                        || c == 't' || c == 'T' || c == 'y' || c == 'Y');
}

/* Longer than any number worth writing out. One longer is refused. */
#define NUMBER_MAX 64

/*
 * A number's characters, as far as `end` or the field width, into `buf`.
 *
 * Returns how many, or -1 when there are more than fit: refused rather than
 * truncated, because parsing a prefix would leave the rest of the digits for
 * the next directive to misread.
 */
static int copy_number(const char *in, const char *end, int width,
                       int floating, char *buf)
{
    int limit = (width > 0 && width < NUMBER_MAX) ? width : NUMBER_MAX - 1;
    int n = 0;

    while (in + n < end && n < limit
           && numberish((unsigned char)in[n], floating)) {
        buf[n] = in[n];
        n++;
    }

    if (n == limit && (width == 0 || width > limit) && in + n < end
        && numberish((unsigned char)in[n], floating)) {
        return -1;
    }

    buf[n] = '\0';

    return n;
}

int kosmos_vscan(const char *in, size_t len, const char *fmt, va_list ap,
                 size_t *used)
{
    const char *start = in;
    const char *end = in + len;
    int matched = 0;
    int result;

    while (*fmt != '\0') {
        if (is_space((unsigned char)*fmt)) {
            /* Any run of whitespace in the format matches any run in the
             * input, including none. */
            while (in < end && is_space((unsigned char)*in)) {
                in++;
            }

            fmt++;
            continue;
        }

        if (*fmt != '%') {
            if (in == end) {
                result = matched > 0 ? matched : EOF;
                goto done;
            }

            if (*in != *fmt) {
                result = matched;
                goto done;
            }

            in++;
            fmt++;
            continue;
        }

        fmt++;                                  /* past the % */

        {
            int suppress = 0;
            int width = 0;
            char length = 0;
            char conv;

            if (*fmt == '*') {
                suppress = 1;
                fmt++;
            }

            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }

            if (*fmt == 'l') {
                length = 'l';
                fmt++;
            }

            conv = *fmt;

            /* `hh`, `h`, `ll`, `L`, `z`, `j`, `t`, a wide `%ls` or `%lc`,
             * or a format that ends here: none of them is a width this
             * writes correctly, so stop before an argument is touched. */
            if (conv == '\0' || strchr("hlLzjt", conv) != NULL
                || (length == 'l' && (conv == 's' || conv == 'c'))) {
                result = matched;
                goto done;
            }

            fmt++;

            if (conv != 'c') {
                while (in < end && is_space((unsigned char)*in)) {
                    in++;
                }
            }

            if (in == end) {
                result = matched > 0 ? matched : EOF;
                goto done;
            }

            if (conv == '%') {
                if (*in != '%') {
                    result = matched;
                    goto done;
                }

                in++;
                continue;
            }

            if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'o'
                || conv == 'x' || conv == 'X') {
                char buf[NUMBER_MAX];
                char *stop;
                int base = (conv == 'd' || conv == 'u') ? 10
                         : (conv == 'o') ? 8
                         : (conv == 'i') ? 0 : 16;

                if (copy_number(in, end, width, 0, buf) <= 0) {
                    result = matched;
                    goto done;
                }

                if (conv == 'd' || conv == 'i') {
                    long v = strtol(buf, &stop, base);

                    if (stop == buf) {
                        result = matched;
                        goto done;
                    }

                    if (!suppress) {
                        if (length == 'l') {
                            *va_arg(ap, long *) = v;
                        } else {
                            *va_arg(ap, int *) = (int)v;
                        }

                        matched++;
                    }
                } else {
                    unsigned long v = strtoul(buf, &stop, base);

                    if (stop == buf) {
                        result = matched;
                        goto done;
                    }

                    if (!suppress) {
                        if (length == 'l') {
                            *va_arg(ap, unsigned long *) = v;
                        } else {
                            *va_arg(ap, unsigned int *) = (unsigned int)v;
                        }

                        matched++;
                    }
                }

                in += stop - buf;
            } else if (conv == 'f' || conv == 'e' || conv == 'g'
                       || conv == 'E' || conv == 'G') {
                char buf[NUMBER_MAX];
                char *stop;
                double v;

                if (copy_number(in, end, width, 1, buf) <= 0) {
                    result = matched;
                    goto done;
                }

                v = strtod(buf, &stop);

                if (stop == buf) {
                    result = matched;
                    goto done;
                }

                if (!suppress) {
                    if (length == 'l') {
                        *va_arg(ap, double *) = v;
                    } else {
                        *va_arg(ap, float *) = (float)v;
                    }

                    matched++;
                }

                in += stop - buf;
            } else if (conv == 's') {
                char *out = suppress ? NULL : va_arg(ap, char *);
                int n = 0;

                while (in < end && !is_space((unsigned char)*in)
                       && (width == 0 || n < width)) {
                    if (out != NULL) {
                        out[n] = *in;
                    }

                    in++;
                    n++;
                }

                if (out != NULL) {
                    out[n] = '\0';
                    matched++;
                }
            } else if (conv == 'c') {
                char *out = suppress ? NULL : va_arg(ap, char *);
                size_t n = (width > 0) ? (size_t)width : 1;

                if ((size_t)(end - in) < n) {
                    result = matched > 0 ? matched : EOF;
                    goto done;
                }

                if (out != NULL) {
                    memcpy(out, in, n);
                    matched++;
                }

                in += n;
            } else {
                /* Not understood. Stop, rather than guess how many
                 * arguments it would have taken. */
                result = matched;
                goto done;
            }
        }
    }

    result = matched;

done:
    if (used != NULL) {
        *used = (size_t)(in - start);
    }

    return result;
}

int vsscanf(const char *in, const char *fmt, va_list ap)
{
    return kosmos_vscan(in, strlen(in), fmt, ap, NULL);
}

int sscanf(const char *in, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsscanf(in, fmt, ap);
    va_end(ap);

    return n;
}
