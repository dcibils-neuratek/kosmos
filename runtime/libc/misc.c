/*
 * The stubs that exist so headers can be included, plus errno.
 */

#include <errno.h>
#include <locale.h>
#include <time.h>
#include <panic.h>

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
