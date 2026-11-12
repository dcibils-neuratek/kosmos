/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef SCHED_H
#define SCHED_H

/* `sched_yield`, for ported code (`docs/w3.md` W3a): the back of this
 * thread's band, as `kosmos_yield` is. Priorities are Kosmos's bands and
 * are not set through here: the policies are named so ported code
 * compiles, every one has the single priority 0, and a thread reads as
 * SCHED_OTHER at it (`pthread.c`). */

#include <sys/cdefs.h>

__BEGIN_DECLS

int sched_yield(void);

#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2

struct sched_param {
    int sched_priority;
};

int sched_get_priority_min(int policy);
int sched_get_priority_max(int policy);

__END_DECLS

#endif /* SCHED_H */
