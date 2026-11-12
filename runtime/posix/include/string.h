/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_STRING_H
#define POSIX_STRING_H

/* The runtime's, and POSIX's thread-safe `strerror` - libc++'s
 * `std::system_error` asks for it (`docs/w4.md` W4a). */
#include_next <string.h>

#include <stddef.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

/* XSI's: the sentence for `errnum` into `buf`, 0, or ERANGE when it does
 * not fit (what fits is still written). */
int strerror_r(int errnum, char *buf, size_t room);

__END_DECLS

#endif /* POSIX_STRING_H */
