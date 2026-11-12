/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef LOCALE_H
#define LOCALE_H

#include <sys/cdefs.h>
__BEGIN_DECLS

/*
 * There is one locale and it is C. Kosmos has no locale machinery and will
 * not grow one: the whole repository is English and the only thing Lua ever
 * asks about is the decimal point.
 *
 * `lua_getlocaledecpoint` is overridden in the Kosmos build to the constant
 * '.', so localeconv is never actually called. The struct exists because the
 * header is included and the type has to be complete.
 */
struct lconv {
    char *decimal_point;
};

#define LC_ALL      0
#define LC_NUMERIC  1

/*
 * Two more categories to ask about, with the same answer, "C". ICU asks
 * both to choose its default locale - and "C" is `en_US_POSIX` to it,
 * which is what a program gets unless it names one.
 */
#define LC_CTYPE    2
#define LC_MESSAGES 3

struct lconv *localeconv(void);
char *setlocale(int category, const char *locale);

__END_DECLS

#endif /* LOCALE_H */
