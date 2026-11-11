/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef SCHED_H
#define SCHED_H

/* `sched_yield`, for ported code (`docs/w3.md` W3a): the back of this
 * thread's band, as `kosmos_yield` is. Priorities are Kosmos's bands and
 * are not set through here. */

#include <sys/cdefs.h>

__BEGIN_DECLS

int sched_yield(void);

__END_DECLS

#endif /* SCHED_H */
