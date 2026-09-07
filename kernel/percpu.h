#ifndef KERNEL_PERCPU_H
#define KERNEL_PERCPU_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu.h"
#include "kernel.h"

/*
 * The state that belongs to one processor rather than to the machine.
 *
 * **`docs/smp.md` step one**, and every field below was a file-scope global
 * in `thread.c` before it: they are named as per-CPU state now and reached
 * through one function, so there is one place that has to be right rather
 * than five.
 *
 * `CLAUDE.md` claimed a per-CPU struct from the repository's first commit
 * and there was never one; the correction is recorded and this is the other
 * half of it.
 *
 * **Step three fills more than one of these.** A secondary started by
 * `kernel/smp.c` claims its own slot and parks, which is what finally
 * demonstrates that the register below is per-core - step one could only
 * assert it on a machine where every answer was the same answer.
 *
 * **Named `percpu.h` and not `cpu.h`** because `-Ikernel` and
 * `-Iarch/<name>` are both on the compile line: a second `cpu.h` here would
 * shadow the architecture's own for every file in the build, and the first
 * thing it would shadow is this file's include of it. `tests/machine.h`
 * carries the same scar.
 */

/*
 * Four, which is how many `struct percpu` slots exist - **not how many are
 * running threads**, which is `thread_cpu_count` and is one.
 *
 * It was 1 while nothing could start a second core. `smp.c` can now, and a
 * core that arrives needs somewhere to put itself, so the slots have to
 * exist first. Four because that is what `make test` boots and what a Pi 5
 * has; the machine is asked how many it really has and the smaller of the
 * two wins.
 */
#define NR_CPUS     4

struct thread;

struct percpu {
    /*
     * Which processor this is. Zero on a machine with one, and the index
     * into `cpus[]` on any other - so it is the answer to "who am I" that
     * every later piece of this work needs and nothing yet asks.
     */
    unsigned index;

    /*
     * The running thread. **The fundamental one**: everything else in this
     * struct is per-CPU because this is.
     *
     * `thread.c` reaches it as `current`, which is a macro over this field
     * rather than twenty-nine edited call sites. That is Linux's idiom for
     * the same reason it is Linux's: the sites were correct, they say what
     * they mean, and rewriting them would be a large diff whose only
     * content is a change of spelling.
     */
    struct thread *current;

    /* Each core idles independently, so each has its own idle thread. */
    struct thread *idle_thread;

    /*
     * Where the processor's time went, in scheduler ticks.
     *
     * Per core because load is measured per core or it is not measured:
     * summing two cores into one pair of counters gives a number that is
     * neither, and `sysinfo` would report a machine that is 50% busy when
     * one core is pinned and the other asleep.
     */
    unsigned long idle_ticks;
    unsigned long busy_ticks;

    /*
     * A switch is owed on the way out of the current exception.
     *
     * Per-CPU because it is a statement about *this* core's return path.
     * `smp.md` listed six things that have to move and missed this one; it
     * was found by reading `thread.c` for the others, which is the argument
     * for doing the audit against the code rather than against the document.
     */
    volatile bool preempt_pending;
};

/*
 * The one this core is on.
 *
 * Reached through a register rather than an index, which is what makes it
 * a constant-time answer from anywhere - including an exception handler
 * that has not yet worked out what it is handling. `arch/<name>/cpu.h`
 * holds the two lines that read and write it, and the two boards differ
 * in a way worth knowing:
 *
 * **AArch64 has `TPIDR_EL1`, which is banked.** EL0 cannot see it and
 * cannot change it, so it is set once per core at boot and read for ever.
 * No entry path is touched at all, which is why this is done here first.
 *
 * **x86-64 has one `GS` shared between ring 3 and ring 0**, so the same
 * trick needs `swapgs` at every entry and every exit - real surgery on
 * `vectors.S` and `user.S`, and work that belongs with x86's second core
 * rather than before it. Until then that board answers from `cpus[0]`,
 * which is correct for one processor and says so.
 */
struct percpu *this_cpu(void);

/*
 * Claims this processor's slot and makes `this_cpu` work.
 *
 * Called once per core, as early in the boot as anything that might fault:
 * before it, `this_cpu` is undefined, and `thread_current` reads through
 * it. That is the one ordering constraint this file has.
 */
void percpu_init(unsigned index);

/*
 * Another processor's slot, by index, or NULL past the end.
 *
 * `this_cpu` is the fast answer for the core asking; this is how one core
 * looks at another's, which is what a scheduler balancing across them will
 * need and what the suite needs today to check that each secondary claimed
 * *its own* rather than all of them landing on zero.
 */
struct percpu *percpu_at(unsigned index);

#endif /* KERNEL_PERCPU_H */
