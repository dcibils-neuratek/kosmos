/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef SYS_TIME_H
#define SYS_TIME_H

/* `gettimeofday`, for ported code (`docs/w3.md` W3c): `CLOCK_REALTIME` to
 * the microsecond. No time zone - it is ignored, as POSIX allows. */

#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>

__BEGIN_DECLS

struct timeval {
    time_t      tv_sec;
    suseconds_t tv_usec;
};

int gettimeofday(struct timeval *out, void *zone);

/* A file's times are the server's: ENOTSUP (`fd.c`). */
int utimes(const char *path, const struct timeval times[2]);

__END_DECLS

#endif /* SYS_TIME_H */
