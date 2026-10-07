/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_TCC_SYS_TIME_H
#define KOSMOS_TCC_SYS_TIME_H

/* What TinyCC's `<sys/time.h>` is asked for, inside the C Kit alone
 * (`shim.h`): a time of day, which only its timing of a build reads. */
#include <time.h>

struct timeval {
    long tv_sec;
    long tv_usec;
};

#endif /* KOSMOS_TCC_SYS_TIME_H */
