#ifndef LOCALE_H
#define LOCALE_H

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

struct lconv *localeconv(void);
char *setlocale(int category, const char *locale);

#endif /* LOCALE_H */
