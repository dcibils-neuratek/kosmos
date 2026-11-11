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
int kosmos_clock_read(int monotonic, struct timespec *out);

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

    {
        struct timespec ts, mono;
        uint64_t at;

        /* A quarter of a second past a whole one, on both clocks. */
        stub_ticks = (stub_ticks / stub_hz + 1) * stub_hz + stub_hz / 4u;
        at = stub_ticks;
        check(kosmos_clock_read(0, &ts) == 0 && kosmos_clock_read(1, &mono) == 0,
              "both clocks read");
        /* The board says the date in whole seconds, so the date is good to
         * the second it came with, and no closer. */
        check(ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000
              && ts.tv_sec >= (time_t)(stub_epoch + at / stub_hz) - 1
              && ts.tv_sec <= (time_t)(stub_epoch + at / stub_hz),
              "the date is the board's, to its second");
        check(mono.tv_sec == (time_t)(at / stub_hz) && mono.tv_nsec == 250000000,
              "the monotonic clock is the counter, from the start");

        /* Ten days of a 4 GHz counter: the nanoseconds must not overflow. */
        stub_hz = 4000000000u;
        stub_ticks = stub_hz * 864000u + stub_hz / 2u + 4u;
        check(kosmos_clock_read(1, &mono) == 0 && mono.tv_sec == 864000
              && mono.tv_nsec == 500000001, "a fast counter's fraction stays exact");
        stub_hz = 62500000u;
        stub_ticks = at + stub_hz * 61u;
        (void)kosmos_time_now();
    }

    stub_epoch = 0;                         /* a machine with no clock */
    stub_ticks += stub_hz * 61u;
    check(kosmos_time_now() == 0, "no clock is zero, not a count from boot");

    if (failures != 0) {
        return 1;
    }

    printf("PASS: 10 checks on time() and clock_gettime: sysinfo once a minute, the counter between\n");
    return 0;
}
