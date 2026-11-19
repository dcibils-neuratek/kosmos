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
 * '.', so Lua never calls localeconv. libc++'s streams do (`numpunct`,
 * `moneypunct`), so the struct is C's whole, with the C locale's values in
 * it: "." for the decimal point, "" for every other string and CHAR_MAX for
 * every number, which C says means "not available here".
 */
struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
    char int_p_cs_precedes;
    char int_p_sep_by_space;
    char int_n_cs_precedes;
    char int_n_sep_by_space;
    char int_p_sign_posn;
    char int_n_sign_posn;
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

/*
 * And C's other three, for libc++'s `locale`, which names every category
 * a locale has - each answered "C" too.
 */
#define LC_COLLATE  4
#define LC_MONETARY 5
#define LC_TIME     6

struct lconv *localeconv(void);
char *setlocale(int category, const char *locale);

__END_DECLS

#endif /* LOCALE_H */
