#ifndef KERNEL_PERCPU_H
#define KERNEL_PERCPU_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu.h"
#include "kernel.h"

/*
 * The state that belongs to one processor rather than to the machine.
 *
 * **This is step one of `docs/smp.md`, and the whole of it is that nothing
 * behaves differently.** `NR_CPUS` is 1, there is still exactly one core,
 * and every field below was a file-scope global in `thread.c` an hour ago.
 * What changes is that they are now *named* as per-CPU state and reached
 * through one function, so the day a second core exists there is one place
 * that has to become right rather than five.
 *
 * Doing it while it cannot fail is the point. `CLAUDE.md` claimed a per-CPU
 * struct from the repository's first commit and there was never one; the
 * correction is recorded and this is the other half of it.
 *
 * **Named `percpu.h` and not `cpu.h`** because `-Ikernel` and
 * `-Iarch/<name>` are both on the compile line: a second `cpu.h` here would
 * shadow the architecture's own for every file in the build, and the first
 * thing it would shadow is this file's include of it. `tests/machine.h`
 * carries the same scar.
 */

/*
 * How many processors this kernel is built for.
 *
 * One, and every array below is sized by it, so raising it is a real
 * change rather than a flag. It is here rather than in `kernel.h` because
 * it is only meaningful beside the thing it sizes.
 */
#define NR_CPUS     1

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

#endif /* KERNEL_PERCPU_H */
