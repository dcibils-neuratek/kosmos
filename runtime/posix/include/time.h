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

/*
 * The time a process, or a thread, has spent running - named so ported
 * code compiles, and refused by `clock_gettime` with EINVAL: Nebula counts
 * each thread's ticks and does not yet hand a thread its own (`roadmap.md`,
 * W4c's follow-ups). JavaScriptCore's watchdog is what asks.
 */
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3

__END_DECLS

#endif /* POSIX_TIME_H */
