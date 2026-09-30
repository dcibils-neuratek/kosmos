/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `time()` in a process, held to what it costs and what it says
 * (`user/init/clock_user.c`, `testing.md` 18.301).
 *
 * It asked the kernel for the whole of `sysinfo` on every call, and libdom's
 * DOM events made that 66% of a page's load. Here, against a clock this test
 * moves: a thousand calls within a minute read `sysinfo` once; the seconds
 * follow the counter; a minute on it is read again; and a machine with no
 * clock says zero rather than counting from boot.
 */

#include <stdint.h>
#include <stdio.h>
#include <time.h>

uint64_t stub_ticks, stub_epoch, stub_hz;
unsigned stub_sysinfo_calls;

time_t kosmos_time_now(void);

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

int main(void)
{
    unsigned i;

    stub_hz = 62500000u;                    /* TCG's counter */
    stub_epoch = 1790000000u;
    stub_ticks = 12345;

    check(kosmos_time_now() == (time_t)stub_epoch, "the first reading is the epoch");

    for (i = 0; i < 1000; i++) {
        stub_ticks += stub_hz / 100u;       /* ten seconds in all */
        (void)kosmos_time_now();
    }

    check(stub_sysinfo_calls == 1, "a thousand calls in ten seconds read sysinfo once");
    check(kosmos_time_now() == (time_t)(stub_epoch + stub_ticks / stub_hz),
          "and the seconds follow the counter");

    stub_ticks += stub_hz * 61u;
    (void)kosmos_time_now();
    check(stub_sysinfo_calls == 2, "a minute on, sysinfo is read again");
    check(kosmos_time_now() == (time_t)(stub_epoch + stub_ticks / stub_hz),
          "and the clock it gives is the new one");

    stub_epoch = 0;                         /* a machine with no clock */
    stub_ticks += stub_hz * 61u;
    check(kosmos_time_now() == 0, "no clock is zero, not a count from boot");

    if (failures != 0) {
        return 1;
    }

    printf("PASS: 6 checks on time(): sysinfo once a minute, the counter between\n");
    return 0;
}
