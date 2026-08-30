#ifndef TESTS_TEST_H
#define TESTS_TEST_H

#include <stdbool.h>

/*
 * The C self-test layer, for M0 through M3. Before Lua exists there is no
 * other option: an array of functions returning bool, run at the end of
 * boot, printed as TAP over the serial line.
 *
 * It freezes at M2. Once there is an interpreter, new tests are written in
 * Lua and the ones already here stay where they are.
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
