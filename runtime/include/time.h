/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef TIME_H
#define TIME_H

#include <sys/cdefs.h>
__BEGIN_DECLS

#include <stddef.h>

/*
 * <time.h>, as much of it as a process uses.
 *
 * The kernel keeps no wall clock and there will not be one in it: the board
 * keeps the time, the kernel hands it on in `sysinfo`, and a process reads
 * it at `/Devices/clock` - which is why even the clock is a capability in
 * `design.md` §9.2. `time()` is that number (`user/init/clock_user.c`), and
 * zero on a machine with no clock; `clock()` is -1, because a process is
 * not told its own processor time (`user/init/misc_user.c`).
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
 * **Time to the nanosecond, on two clocks** (`docs/webkit.md` W1c): C++'s
 * `std::chrono` and WebKit's `MonotonicTime` and `WallTime` read these.
 *
 * - `CLOCK_REALTIME` is the date - `time()`'s seconds, with the counter's
 *   fraction since the board was last asked - and zero on a machine that
 *   does not know it. The board says the date in whole seconds, so this is
 *   good to a second and no closer: for how long something took, the other;
 * - `CLOCK_MONOTONIC` is the counter (`kosmos_ticks`), which measures and
 *   never goes back, from whenever the machine started.
 *
 * Which is which is in the name, as `CLAUDE.md`'s two clocks want, and a
 * clock id that is neither is refused with -1 and `EINVAL`. C11's
 * `timespec_get` is the first of them under the standard's name.
 */
struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

typedef int clockid_t;

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#define TIME_UTC        1

int clock_gettime(clockid_t which, struct timespec *out);
int timespec_get(struct timespec *out, int base);

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

__END_DECLS

#endif /* TIME_H */
