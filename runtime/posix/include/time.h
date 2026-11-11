/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_TIME_H
#define POSIX_TIME_H

/*
 * **The runtime's `<time.h>`, and `nanosleep`** (`docs/w3.md` W3c), for
 * ported code only. Everything else - both clocks, `clock_gettime`, the
 * broken-down time - is the runtime's, reached with `#include_next`, so
 * there is one copy of it.
 */

#include_next <time.h>

#include <sys/cdefs.h>

__BEGIN_DECLS

/* At least `wanted`, on the monotonic clock; `left`, when given, is set to
 * nought, since nothing interrupts a sleep here (there are no signals). */
int nanosleep(const struct timespec *wanted, struct timespec *left);

__END_DECLS

#endif /* POSIX_TIME_H */
