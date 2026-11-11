/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_INTERNAL_H
#define POSIX_INTERNAL_H

/*
 * What the POSIX layer's own files share (`docs/w3.md`), and nothing a
 * port sees: it is not in `runtime/posix/include/`.
 */

/* The scheduler's ticks a second - every wait's unit - from `SYS_SYSINFO`,
 * asked once (`time.c`). */
unsigned long posix_tick_hz(void);

/* Ticks for `ns` nanoseconds, rounded up and at least one when `ns` is
 * above nought: a wait is never shorter than it was asked to be. */
unsigned long posix_ticks_for_ns(long long ns);

#endif /* POSIX_INTERNAL_H */
