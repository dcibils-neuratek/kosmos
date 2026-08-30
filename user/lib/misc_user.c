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
