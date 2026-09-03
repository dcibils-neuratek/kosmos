/*
 * The pieces of the libc that differ inside a process.
 *
 * errno and the locale are the same in shape and different in where they
 * live. What is absent is time(): a process has no clock, cannot read the
 * counter, and has nothing to ask yet. Lua's uses of it are redirected in
 * kosmos_lua.h, and anything else calling it is a link error, which is the
 * right answer to a question the system cannot answer.
 */

#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>

/*
 * One per process, which is what `design.md` §17.3 asks for. Here it is
 * literally true rather than aspirational: this is a different address
 * space, so there is no other errno for it to be confused with.
 */
static int errno_storage;

int *__errno(void)
{
    return &errno_storage;
}

static char decimal_point[] = ".";
static struct lconv c_locale = { decimal_point };

struct lconv *localeconv(void)
{
    return &c_locale;
}

char *setlocale(int category, const char *locale)
{
    (void)category;
    (void)locale;
    return (char *)"C";
}

/*
 * Here rather than in `runtime/libc/misc.c`, which is the *kernel* side's.
 *
 * The link error said so: these went into that file first and the user
 * image would not link, because it does not compile it. `USER_LIBC` names
 * what a process gets, and this file is the userland half of the pair.
 */

/*
 *--------------------------------------------------------------------------
 * Conversion and sorting, added because a link error asked.
 *--------------------------------------------------------------------------
 */

long strtol(const char *s, char **end, int base)
{
    const char *at = s;
    long value = 0;
    int negative = 0;

    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') {
        at++;
    }

    if (*at == '+' || *at == '-') {
        negative = (*at == '-');
        at++;
    }

    /* `base == 0` means "work it out from the prefix", which is what makes
     * this usable for the `0x` in a config file as well as for a decimal. */
    if ((base == 0 || base == 16)
        && at[0] == '0' && (at[1] == 'x' || at[1] == 'X')) {
        base = 16;
        at += 2;
    } else if (base == 0) {
        base = (at[0] == '0') ? 8 : 10;
    }

    for (;;) {
        int digit;

        if (*at >= '0' && *at <= '9') {
            digit = *at - '0';
        } else if (*at >= 'a' && *at <= 'z') {
            digit = *at - 'a' + 10;
        } else if (*at >= 'A' && *at <= 'Z') {
            digit = *at - 'A' + 10;
        } else {
            break;
        }

        if (digit >= base) {
            break;
        }

        value = value * base + digit;
        at++;
    }

    /*
     * No overflow detection and no ERANGE.
     *
     * Said out loud rather than left to be discovered: this does not set
     * errno and does not saturate at LONG_MAX. Every caller here is parsing
     * a number it wrote itself - a command line argument, a field in a
     * config file - and none of them check. When something needs to parse a
     * number from somewhere it does not control, this is the function that
     * has to grow the check, and it should grow it then rather than carry
     * an unused one now.
     */
    if (end != NULL) {
        *end = (char *)at;
    }

    return negative ? -value : value;
}

int atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}

/*
 * There is no environment, so there is nothing in it.
 *
 * Not a stub waiting to be filled in: `CLAUDE.md` forbids a POSIX
 * personality, and an environment is one - a per-process bag of global
 * names that a child inherits without being handed anything. What this
 * system has instead is the namespace, where what you were not given you
 * cannot reach. Returning NULL is the correct and permanent answer, and
 * every caller already handles it because on a real system a variable may
 * simply not be set.
 */
char *getenv(const char *name)
{
    (void)name;

    return NULL;
}

/*
 * Insertion sort.
 *
 * `qsort` is the name in the standard and this is not quicksort, which is
 * worth saying rather than hiding: the arrays it is asked to sort here are
 * tens of elements, insertion sort has no recursion and no stack, and the
 * kernel is not the only place in this system where an unbounded stack is a
 * problem. If something ever sorts a large array through this, the profile
 * will say so and the answer will be to write the better algorithm then.
 */
void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *))
{
    unsigned char *a = base;
    size_t i, j;

    for (i = 1; i < count; i++) {
        for (j = i; j > 0; j--) {
            unsigned char *lhs = a + (j - 1) * size;
            unsigned char *rhs = a + j * size;
            size_t k;

            if (compare(lhs, rhs) <= 0) {
                break;
            }

            /* Swapped a byte at a time, because the element size is a
             * runtime value and there is no temporary big enough for it. */
            for (k = 0; k < size; k++) {
                unsigned char t = lhs[k];

                lhs[k] = rhs[k];
                rhs[k] = t;
            }
        }
    }
}

/*
 * There is no shell to hand a command line to, and there will not be one.
 *
 * `system()` is a POSIX personality in a single function: it takes a string,
 * finds an interpreter by a global name, and runs it with this process's
 * authority. `CLAUDE.md` forbids exactly that shape. Kosmos starts a program
 * by asking a server that already holds the right to start one.
 *
 * -1 is "the command could not be run", which is the answer, and every
 * caller has to handle it because on a real system the shell may be missing.
 */
int system(const char *command)
{
    (void)command;

    return -1;
}

/*
 * Making a directory needs a path, and a path needs a tree to be in.
 *
 * -1 with errno untouched: the caller checks, says so, and goes on without
 * whatever it wanted the directory for - which for Doom is a place to put
 * savegames it also cannot write. See `<sys/stat.h>`.
 */
int mkdir(const char *path, unsigned int mode)
{
    (void)path; (void)mode;

    return -1;
}

/*
 *--------------------------------------------------------------------------
 * `sscanf`, the small half of it.
 *
 * Added because a link error asked - Doom parses its config file with it -
 * and written properly rather than stubbed, which was the tempting shortcut:
 * there is no config file on this system, so a stub returning zero would be
 * correct today and a trap the first time anything else called it. A
 * function that silently matches nothing is worse than one that is missing,
 * because the missing one fails at the link.
 *
 * What it handles: whitespace, literal characters, and `%d %i %u %x %c %s`
 * with an optional field width, plus `%f`/`%lf` through `strtod`. `%*`
 * suppression is honoured. Anything else stops the scan and returns what
 * matched so far, which is what the standard says to do and means an
 * unsupported directive loses a field rather than misreading one.
 *--------------------------------------------------------------------------
 */

#include <stdarg.h>
#include <stdlib.h>

static int is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r'
        || c == '\v' || c == '\f';
}

int vsscanf(const char *in, const char *fmt, va_list ap)
{
    int matched = 0;

    while (*fmt != '\0') {
        if (is_space((unsigned char)*fmt)) {
            /* Any run of whitespace in the format matches any run in the
             * input, including none. */
            while (is_space((unsigned char)*in)) {
                in++;
            }

            fmt++;
            continue;
        }

        if (*fmt != '%') {
            if (*in != *fmt) {
                return matched;
            }

            in++;
            fmt++;
            continue;
        }

        fmt++;                              /* past the % */

        {
            int suppress = 0;
            int width = 0;
            char conv;

            if (*fmt == '*') {
                suppress = 1;
                fmt++;
            }

            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }

            /* Length modifiers, which change the argument's size and not
             * what is parsed. `%lf` is the one Doom uses. */
            while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') {
                fmt++;
            }

            conv = *fmt++;

            if (conv == '%') {
                if (*in != '%') {
                    return matched;
                }

                in++;
                continue;
            }

            if (conv != 'c') {
                while (is_space((unsigned char)*in)) {
                    in++;
                }
            }

            if (*in == '\0') {
                return matched > 0 ? matched : -1;
            }

            if (conv == 'd' || conv == 'i' || conv == 'u'
                || conv == 'x' || conv == 'X') {
                char *end;
                long v = strtol(in, &end, (conv == 'x' || conv == 'X') ? 16
                                          : (conv == 'i' ? 0 : 10));

                if (end == in) {
                    return matched;
                }

                in = end;

                if (!suppress) {
                    *va_arg(ap, int *) = (int)v;
                    matched++;
                }
            } else if (conv == 'f' || conv == 'e' || conv == 'g') {
                char *end;
                double v = strtod(in, &end);

                if (end == in) {
                    return matched;
                }

                in = end;

                if (!suppress) {
                    *va_arg(ap, double *) = v;
                    matched++;
                }
            } else if (conv == 's') {
                char *out = suppress ? NULL : va_arg(ap, char *);
                int n = 0;

                while (*in != '\0' && !is_space((unsigned char)*in)
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
                int n = (width > 0) ? width : 1;
                int i;

                for (i = 0; i < n && *in != '\0'; i++) {
                    if (out != NULL) {
                        out[i] = *in;
                    }

                    in++;
                }

                if (out != NULL) {
                    matched++;
                }
            } else {
                /* Not understood. Stop, rather than guess how many
                 * arguments it would have taken. */
                return matched;
            }
        }
    }

    return matched;
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

/* `atof` is `strtod` without the end pointer, and there is no error to
 * report: it is defined to return zero for text that is not a number. */
double atof(const char *s)
{
    return strtod(s, NULL);
}
