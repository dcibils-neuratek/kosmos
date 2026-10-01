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
 * `%d %i %u %o %x %X %c %s %f %e %g %[...] %n` with an optional field width
 * and `*` suppression. Anything else stops the scan and returns what matched
 * so far, which is what the standard says to do, and loses a field rather
 * than misreading one.
 *
 * `%[` and `%n` arrived with SVG (`roadmap.md` 6zz j5): libsvgtiny reads
 * a path's commands with `" %1[MmLl] %f %f %n"`, and without them every
 * `<path>` was refused at its first letter - circles and rectangles, which
 * it builds without scanning, were drawn and nothing else was.
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
 * Is `c` in the scanset [set, close)? Ranges are `a-z`; a `-` first or
 * last is itself, as is a `]` first, which the caller has already stepped
 * over when finding `close`.
 */
static int in_set(int c, const char *set, const char *close)
{
    const char *p = set;

    while (p < close) {
        if (p + 2 < close && p[1] == '-') {
            if (c >= (unsigned char)p[0] && c <= (unsigned char)p[2]) {
                return 1;
            }

            p += 3;
        } else {
            if (c == (unsigned char)*p) {
                return 1;
            }

            p++;
        }
    }

    return 0;
}

/* Longer than any number worth writing out. One longer is refused. */
#define NUMBER_MAX 64

static int is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static int is_hex(int c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

/*
 * How long the number at `in` is, by its grammar: a sign, digits - hex
 * ones after `0x`, or when `hex` - and for a float a point, more digits and
 * an exponent, or `inf`, `infinity` or `nan`. `strtol` and `strtod` still
 * decide what it is worth; this decides only how much of the input they
 * are shown.
 *
 * **It used to copy every character that could appear in a number**, and
 * a minus sign and the letters a to f all can. So an SVG path written the
 * compact way - `c-1.2-3.4-5.6...`, every number its own sign's - was one
 * run of "number" as long as the path, refused past 63 characters, and
 * the path stopped there: Wikipedia's wordmark drew a W and "of the f".
 */
static size_t number_length(const char *in, const char *end, int floating,
                            int hex)
{
    static const char *const words[] = { "infinity", "inf", "nan" };
    const char *p = in;
    size_t i;

    if (p < end && (*p == '+' || *p == '-')) {
        p++;
    }

    if (floating) {
        for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
            size_t k = strlen(words[i]), j;

            for (j = 0; j < k && p + j < end
                        && lower((unsigned char)p[j]) == words[i][j]; j++) {
            }

            if (j == k) {
                return (size_t)(p + k - in);
            }
        }
    }

    if ((hex || floating) && end - p >= 2 && p[0] == '0'
        && lower((unsigned char)p[1]) == 'x') {
        p += 2;

        while (p < end && is_hex((unsigned char)*p)) {
            p++;
        }

        if (floating) {
            if (p < end && *p == '.') {
                p++;

                while (p < end && is_hex((unsigned char)*p)) {
                    p++;
                }
            }

            if (p < end && lower((unsigned char)*p) == 'p') {
                const char *q = p + 1;

                if (q < end && (*q == '+' || *q == '-')) {
                    q++;
                }

                if (q < end && is_digit((unsigned char)*q)) {
                    while (q < end && is_digit((unsigned char)*q)) {
                        q++;
                    }

                    p = q;
                }
            }
        }

        return (size_t)(p - in);
    }

    while (p < end && (hex ? is_hex((unsigned char)*p)
                           : is_digit((unsigned char)*p))) {
        p++;
    }

    if (floating) {
        if (p < end && *p == '.') {
            p++;

            while (p < end && is_digit((unsigned char)*p)) {
                p++;
            }
        }

        if (p < end && lower((unsigned char)*p) == 'e') {
            const char *q = p + 1;

            if (q < end && (*q == '+' || *q == '-')) {
                q++;
            }

            if (q < end && is_digit((unsigned char)*q)) {
                while (q < end && is_digit((unsigned char)*q)) {
                    q++;
                }

                p = q;
            }
        }
    }

    return (size_t)(p - in);
}

/*
 * A number's characters, as far as `end` or the field width, into `buf`.
 *
 * Returns how many, or -1 when there are more than fit: refused rather than
 * truncated, because parsing a prefix would leave the rest of the digits for
 * the next directive to misread.
 */
static int copy_number(const char *in, const char *end, int width,
                       int floating, int hex, char *buf)
{
    size_t n = number_length(in, end, floating, hex);

    if (width > 0 && (size_t)width < n) {
        n = (size_t)width;
    }

    if (n >= NUMBER_MAX) {
        return -1;
    }

    memcpy(buf, in, n);
    buf[n] = '\0';

    return (int)n;
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
            const char *set = NULL, *close = NULL;
            int negate = 0;

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
                || (length == 'l'
                    && (conv == 's' || conv == 'c' || conv == '['))) {
                result = matched;
                goto done;
            }

            fmt++;

            /*
             * `%n`: how much has been read so far. Not a conversion - it
             * skips no white space, counts as no match, and answers at the
             * end of the input as anywhere else.
             */
            if (conv == 'n') {
                if (!suppress) {
                    if (length == 'l') {
                        *va_arg(ap, long *) = (long)(in - start);
                    } else {
                        *va_arg(ap, int *) = (int)(in - start);
                    }
                }

                continue;
            }

            /* `%[`'s set runs to the first `]` after its first character,
             * or after `^` and its first; a set with no end is not one. */
            if (conv == '[') {
                set = fmt;
                negate = *set == '^';

                if (negate) {
                    set++;
                }

                close = strchr(set + (*set == ']'), ']');

                if (close == NULL) {
                    result = matched;
                    goto done;
                }

                fmt = close + 1;
            }

            if (conv != 'c' && conv != '[') {
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

                if (copy_number(in, end, width, 0, base == 16 || base == 0,
                                buf) <= 0) {
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

                if (copy_number(in, end, width, 1, 0, buf) <= 0) {
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
            } else if (conv == '[') {
                char *out = suppress ? NULL : va_arg(ap, char *);
                int n = 0;

                while (in < end && (width == 0 || n < width)
                       && in_set((unsigned char)*in, set, close) != negate) {
                    if (out != NULL) {
                        out[n] = *in;
                    }

                    in++;
                    n++;
                }

                /* Nothing in the set is a failure to match, not an empty
                 * string. */
                if (n == 0) {
                    result = matched;
                    goto done;
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
