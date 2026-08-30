#ifndef TESTS_TEST_H
#define TESTS_TEST_H

#include <stdbool.h>

/*
 * The C self-test layer: an array of functions returning bool, run at the
 * end of boot and printed as TAP over the serial line.
 *
 * What belongs here is what can only be checked from inside the kernel -
 * registers, page tables, the scheduler, the state of a thread. Anything
 * about Lua is a process now: `user/tests/luatest.lua` holds the assertions
 * and the driver on this side turns an exit code into a TAP line, so the
 * plan, the numbering and the names all stay in one place.
 *
 * Only built into the image when KOSMOS_TEST is defined, so it costs the
 * normal build nothing.
 */

struct test {
    const char *name;
    bool (*fn)(void);
};

/* Runs the suite, prints the TAP stream, and exits the guest through
 * semihosting with 0 if everything passed and 1 otherwise. Never returns. */
void tests_run(void);

#endif /* TESTS_TEST_H */
