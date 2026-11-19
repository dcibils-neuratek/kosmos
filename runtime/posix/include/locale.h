/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_LOCALE_H
#define POSIX_LOCALE_H

/*
 * The runtime's `<locale.h>`, and POSIX's `locale_t` over the one locale
 * Kosmos has, "C" (`docs/w5.md` W5e, for Skia's `SkAutoLocaleSetter`):
 * `newlocale` gives it for "C", "POSIX" or "", and refuses any other name
 * with ENOENT; `uselocale` keeps nothing, since there is nothing to
 * switch between, and says "C" was in use; `freelocale` has nothing to
 * free. A program asking for "C" - the reason most ask at all - gets what
 * it asked for.
 */

#include_next <locale.h>

#include <sys/cdefs.h>

__BEGIN_DECLS

typedef struct kosmos_locale *locale_t;

#define LC_CTYPE_MASK    (1 << LC_CTYPE)
#define LC_NUMERIC_MASK  (1 << LC_NUMERIC)
#define LC_MESSAGES_MASK (1 << LC_MESSAGES)
#define LC_COLLATE_MASK  (1 << LC_COLLATE)
#define LC_MONETARY_MASK (1 << LC_MONETARY)
#define LC_TIME_MASK     (1 << LC_TIME)
#define LC_ALL_MASK      (LC_CTYPE_MASK | LC_NUMERIC_MASK | LC_MESSAGES_MASK | \
                          LC_COLLATE_MASK | LC_MONETARY_MASK | LC_TIME_MASK)

#define LC_GLOBAL_LOCALE ((locale_t)-1)

locale_t newlocale(int mask, const char *name, locale_t base);
locale_t uselocale(locale_t locale);
void     freelocale(locale_t locale);

__END_DECLS

#endif /* POSIX_LOCALE_H */
