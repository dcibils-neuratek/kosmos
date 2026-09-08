/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
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
 * `rand` and `srand`, which is the generator printed in the C standard.
 *
 * A linear congruential generator with the constants from C99 7.20.2.2's
 * own example, so that what it produces is exactly what the standard says a
 * conforming implementation may produce, and nobody has to wonder whether
 * this one is unusual.
 *
 * **It is not for anything that must not be guessed.** Sixteen bits of
 * output from a thirty-two bit state, entirely determined by the seed:
 * `crypto.c` is where randomness with a requirement on it lives. This is
 * for a debug overlay picking a colour, which is what asked for it.
 */
static unsigned long rand_state = 1;

int rand(void)
{
    rand_state = rand_state * 1103515245UL + 12345UL;

    return (int)((rand_state / 65536UL) % 32768UL);
}

void srand(unsigned seed)
{
    rand_state = seed;
}
