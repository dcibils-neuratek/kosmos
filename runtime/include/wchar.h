/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef WCHAR_H
#define WCHAR_H

#include <sys/cdefs.h>
__BEGIN_DECLS

#include <stddef.h>

/*
 * **The types of wide characters, and none of their functions** (`docs/
 * webkit.md` W1c).
 *
 * C++'s library is built without wide characters here - text in Kosmos is
 * UTF-8 bytes, and WebKit keeps its own UTF-16 in `WTF::String` - but its
 * headers still name `mbstate_t`, the state of a conversion, and `wint_t`.
 * So the types are declared and the functions are not: a program that
 * calls `mbrtowc` fails at the link, with the name of what it wanted, as
 * `unistd.h` has it.
 */
#ifndef __cplusplus
typedef __WCHAR_TYPE__ wchar_t;
#endif
typedef __WINT_TYPE__  wint_t;

typedef struct {
    unsigned int  count;        /* bytes of a character seen so far */
    unsigned int  value;        /* and what they say */
} mbstate_t;

#define WCHAR_MIN   (-__WCHAR_MAX__ - 1)
#define WCHAR_MAX   __WCHAR_MAX__
#define WEOF        ((wint_t)-1)

/* The one the compiler calls of its own accord (`runtime/libc/string.c`). */
size_t wcslen(const wchar_t *s);

__END_DECLS

#endif /* WCHAR_H */
