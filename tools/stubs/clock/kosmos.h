/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `kosmos.h` as `user/init/clock_user.c` sees it, for `tools/test_clock.c` on
 * the Mac: the two calls it makes, counted, and a clock the test moves.
 */
#ifndef KOSMOS_CLOCK_STUB_H
#define KOSMOS_CLOCK_STUB_H

#include <stdint.h>
#include <string.h>

struct sysinfo {
    uint64_t counter_hz;
    uint64_t epoch;
};

extern uint64_t stub_ticks, stub_epoch, stub_hz;
extern unsigned stub_sysinfo_calls;

static inline unsigned long kosmos_ticks(void)
{
    return (unsigned long)stub_ticks;
}

static inline long kosmos_sysinfo(struct sysinfo *out)
{
    stub_sysinfo_calls++;
    memset(out, 0, sizeof(*out));
    out->counter_hz = stub_hz;
    /* A board with a clock counts on; one without says zero. */
    out->epoch = stub_epoch ? stub_epoch + stub_ticks / (stub_hz ? stub_hz : 1) : 0;
    return 0;
}

#endif
