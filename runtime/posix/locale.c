/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **`locale_t` over "C"** (`<locale.h>` in the POSIX layer; `docs/w5.md`
 * W5e). Kosmos has one locale, so a `locale_t` is a pointer to the one
 * object that stands for it, and a program may ask for it, use it and
 * give it back exactly as on a system with many.
 */

#include <errno.h>
#include <locale.h>
#include <string.h>

struct kosmos_locale {
    int unused;
};

static struct kosmos_locale c_locale;

locale_t newlocale(int mask, const char *name, locale_t base)
{
    (void)mask;
    (void)base;

    if (name == NULL) {
        errno = EINVAL;
        return (locale_t)0;
    }

    if (strcmp(name, "C") != 0 && strcmp(name, "POSIX") != 0 && name[0] != '\0') {
        errno = ENOENT;
        return (locale_t)0;
    }

    return &c_locale;
}

locale_t uselocale(locale_t locale)
{
    (void)locale;
    return LC_GLOBAL_LOCALE;
}

void freelocale(locale_t locale)
{
    (void)locale;
}
