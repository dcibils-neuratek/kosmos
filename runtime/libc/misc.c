/*
 * The stubs that exist so headers can be included, plus errno.
 */

#include <errno.h>
#include <locale.h>
#include <time.h>
#include <panic.h>
#include <stdlib.h>
#include <stddef.h>

/*
 * errno's storage. Today there is one of it because there is one thread.
 *
 * `design.md` §17.3 says this has to be per process, and calls it a detail
 * that causes bugs months later: with coroutines a shared errno is read by
 * whoever happens to run next. Reaching it through a function from the start
 * means that when the storage moves into the process state at M4, not one
 * caller changes.
 *
 * newlib's libm calls __errno for its own reasons, so its convention and the
 * design's requirement turn out to be the same one.
 */
static int errno_storage;

int *__errno(void)
{
    return &errno_storage;
}

/*
 * There is one locale and it is C. Lua's uses of localeconv are all
 * overridden in the Kosmos build, so this exists to satisfy the linker and
 * to be honest if something ever does call it.
 */
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
 * There is no wall clock, and the kernel is not where one belongs: time is a
 * resource reached through a namespace, at /dev/clock, which is why even the
 * clock is a capability in `design.md` §9.2.
 *
 * Lua wants time() only to seed hash randomisation, and that seed is
 * overridden in the Kosmos build to use the cycle counter. Anything else
 * calling this is asking a question the kernel cannot answer, and getting a
 * plausible wrong number back would be worse than stopping.
 */
time_t time(time_t *t)
{
    (void)t;
    panic("time(): there is no wall clock. Read /dev/clock instead.");
}

clock_t clock(void)
{
    panic("clock(): there is no wall clock. Read /dev/clock instead.");
}

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
