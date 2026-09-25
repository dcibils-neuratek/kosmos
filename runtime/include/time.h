/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef TIME_H
#define TIME_H

#include <stddef.h>

/*
 * Enough of <time.h> for the headers that include it to compile.
 *
 * There is no wall clock in Kosmos and there will not be one in the kernel:
 * time is a resource a process reaches through its namespace, at /dev/clock,
 * which is why even the clock is a capability in `design.md` §9.2.
 *
 * Lua wants time() only to seed its hash randomisation, and that seed is
 * overridden in the Kosmos build to use the counter instead.
 */
typedef long time_t;
typedef long clock_t;

#define CLOCKS_PER_SEC  1000000L

time_t time(time_t *t);
clock_t clock(void);

/*
 * Broken-down time, which is arithmetic and not a clock: which date and
 * hour a count of seconds names, and back. `runtime/libc/time.c` has it and
 * says why it arrived - FFmpeg - and what "local" means on a machine that
 * does not know where it is.
 */
struct tm {
    int tm_sec;     /* 0-60, the 60 a leap second */
    int tm_min;     /* 0-59 */
    int tm_hour;    /* 0-23 */
    int tm_mday;    /* 1-31 */
    int tm_mon;     /* 0-11 */
    int tm_year;    /* years since 1900 */
    int tm_wday;    /* 0-6, Sunday first */
    int tm_yday;    /* 0-365 */
    int tm_isdst;   /* always 0 here */
};

struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);
time_t     mktime(struct tm *tm);
size_t     strftime(char *out, size_t size, const char *format,
                    const struct tm *tm);

#endif /* TIME_H */
