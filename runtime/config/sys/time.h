/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `sys/time.h`, for expat alone - this directory is on its include path and
 * nobody else's (`runtime/config/expat_config.h`).
 *
 * expat includes it for `gettimeofday`, which it calls only to salt its hash
 * tables when the system has no `arc4random_buf`. Kosmos has one, so that
 * code is not compiled and nothing here is ever reached: the header has to
 * exist, and has nothing to say. It used to be found among newlib's headers,
 * which ARM's toolchain ships beside the compiler and this system does not
 * use - x86's has none, and that is how it was noticed.
 */

#ifndef KOSMOS_EXPAT_SYS_TIME_H
#define KOSMOS_EXPAT_SYS_TIME_H

#endif /* KOSMOS_EXPAT_SYS_TIME_H */
