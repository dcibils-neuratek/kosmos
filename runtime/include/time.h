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

#endif /* TIME_H */
