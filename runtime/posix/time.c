/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Time, for ported code** (`docs/w3.md` W3c): sleeping, `gettimeofday`,
 * and the one conversion every wait in the layer uses - nanoseconds to the
 * scheduler's ticks, the unit `SYS_SLEEP` and the futex count in
 * (`CLAUDE.md`, *Two clocks*). The clocks themselves are the runtime's
 * (`clock_gettime`, `user/init/clock_user.c`).
 *
 * A sleep is held to the monotonic clock rather than trusted to its ticks:
 * a sleep of one tick can wake at the next tick's edge, which may be almost
 * no time at all, so it sleeps again until the clock says the time asked
 * for has gone. Never shorter, which is what POSIX promises.
 */

#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "kosmos.h"
#include "posix.h"

unsigned long posix_tick_hz(void)
{
    static unsigned long hz;

    if (hz == 0) {
        struct sysinfo info;

        hz = (kosmos_sysinfo(&info) == 0 && info.tick_hz != 0) ? info.tick_hz : 250;
    }

    return hz;
}

unsigned long posix_ticks_for_ns(long long ns)
{
    if (ns <= 0) {
        return 0;
    }

    return (unsigned long)((ns * (long long)posix_tick_hz() + 999999999LL) / 1000000000LL);
}

static long long monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }

    return (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
}

int nanosleep(const struct timespec *wanted, struct timespec *left)
{
    long long until;

    if (wanted == NULL || wanted->tv_sec < 0 || wanted->tv_nsec < 0
        || wanted->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }

    until = monotonic_ns() + (long long)wanted->tv_sec * 1000000000LL + wanted->tv_nsec;

    for (;;) {
        long long now = monotonic_ns();

        if (now >= until) {
            break;
        }

        kosmos_sleep(posix_ticks_for_ns(until - now));
    }

    if (left != NULL) {
        left->tv_sec = 0;
        left->tv_nsec = 0;
    }

    return 0;
}

unsigned int sleep(unsigned int seconds)
{
    struct timespec t = { (time_t)seconds, 0 };

    (void)nanosleep(&t, NULL);
    return 0;
}

int usleep(useconds_t microseconds)
{
    struct timespec t = { (time_t)(microseconds / 1000000u),
                          (long)(microseconds % 1000000u) * 1000L };

    return nanosleep(&t, NULL);
}

int gettimeofday(struct timeval *out, void *zone)
{
    struct timespec now;

    (void)zone;

    if (out == NULL) {
        return 0;
    }

    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return -1;
    }

    out->tv_sec = now.tv_sec;
    out->tv_usec = (suseconds_t)(now.tv_nsec / 1000);
    return 0;
}
