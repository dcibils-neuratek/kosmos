#ifndef BENCH_BENCH_H
#define BENCH_BENCH_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The guest side of the benchmark suite, built only into the bench image so
 * it costs the normal one nothing.
 *
 * Numbers are counter ticks, and they mean something only under QEMU's
 * `-icount`, where the clock is a function of instructions retired rather
 * than of host time. `testing.md` §18.3: this measures work, not time, and
 * it exists to detect change rather than to say whether anything is fast.
 */

struct bench_result {
    uint64_t      total;        /* counter ticks across the whole run */
    unsigned long iterations;   /* operations performed */
};

struct benchmark {
    const char *name;
    bool (*run)(struct bench_result *out);
};

/* Runs everything and exits the guest through semihosting. Never returns. */
void bench_run(void);

#endif /* BENCH_BENCH_H */
