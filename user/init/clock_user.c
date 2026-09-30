/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What time it is, for a process: `time()`.
 *
 * `runtime/libc/misc.c` has a `time` too and the user image does not compile
 * that file - it is the kernel side's.
 *
 * `sysinfo.epoch` is seconds since 1970 from the board's clock, and zero on a
 * machine that has none. Zero stays the answer there rather than a number
 * counted from boot: a caller can tell "this machine does not know" from a
 * date, and cannot tell it from a date that is wrong.
 *
 * **Read once a minute and counted in between.** It asked the kernel for the
 * whole of `sysinfo` - every processor's counts, the bus, the memory - on
 * every call, 31 us under TCG, to read one number. libdom stamps every DOM
 * event with `time(NULL)` and fires several for every node the parser
 * inserts, so loading Wikipedia's Dam article spent **66% of the machine's
 * busy time here** (the profile of 30 September; `roadmap.md` 6zz g). Diego:
 * "make the browser fast please, a slow browser is unusable". So the epoch is
 * read with the counter beside it, and the counter - one cheap call - says
 * how far it has moved since; `sysinfo` again once a minute, so a clock set
 * in the meantime is followed. Two threads refreshing at once each store a
 * whole reading, which costs only a second read. `tools/test_clock.c` holds
 * it to that on the Mac.
 *
 * **The `#undef` is not decoration.** `kosmos_lua.h` is forced in front of
 * every user translation unit and defines `time(t)` as `kosmos_lua_time(t)`,
 * so without it the definition below would compile as a second
 * `kosmos_lua_time` and collide with the real one in `lua_glue.c`.
 */

#include <stdint.h>
#include <time.h>

#include "kosmos.h"

static uint64_t clock_epoch, clock_at, clock_hz;

time_t kosmos_time_now(void)
{
    uint64_t now_ticks = kosmos_ticks();

    if (clock_hz == 0 || now_ticks - clock_at >= clock_hz * 60u) {
        struct sysinfo info;

        if (kosmos_sysinfo(&info) == 0 && info.counter_hz != 0) {
            clock_epoch = info.epoch;
            clock_at = now_ticks;
            clock_hz = info.counter_hz;
        }
    }

    if (clock_hz == 0 || clock_epoch == 0) {
        return 0;               /* this machine does not know */
    }

    return (time_t)(clock_epoch + (now_ticks - clock_at) / clock_hz);
}

#ifndef KOSMOS_CLOCK_TEST
#undef time

time_t time(time_t *t)
{
    time_t now = kosmos_time_now();

    if (t != NULL) {
        *t = now;
    }

    return now;
}
#endif
