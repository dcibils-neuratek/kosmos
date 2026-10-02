/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KERNEL_SPINLOCK_H
#define KERNEL_SPINLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu.h"
#include "percpu.h"

/*
 * The kernel's only lock, and the first one it has ever had.
 *
 * **What this replaces is a sentence.** Mutual exclusion in Nebula has meant
 * *there is one core, and the kernel runs with interrupts masked* since the
 * first commit - which is not a weak design, it is the correct one for a
 * uniprocessor and it is why `ipc.c` is eight hundred lines instead of two
 * thousand. `docs/smp.md` step five is where it stops being true.
 *
 * --------------------------------------------------------------------
 * Every lock masks interrupts, and that is deliberate.
 * --------------------------------------------------------------------
 *
 * There is no second flavour that leaves them on. The reason is specific
 * rather than cautious: **the structures worth locking here are touched from
 * both a syscall and an interrupt handler.** The runqueue is the clearest -
 * `thread_yield` reaches it from a syscall and `thread_tick` reaches it from
 * the timer - so a core holding it with interrupts enabled can be
 * interrupted into code that wants the same lock, on the same core, which no
 * amount of spinning resolves. That is a self-deadlock and it is silent.
 *
 * Masking on this core is enough. A *different* core taking the interrupt
 * spins on the lock and gets it when the holder is done, which is what a
 * spinlock is for.
 *
 * The cost is real and it is bounded: interrupts are off for as long as the
 * critical section, so every one of them has to be short. That is a
 * constraint on the code that takes the lock rather than on the lock, and it
 * is the same constraint this kernel already lived under when *all* of it
 * ran masked.
 *
 * --------------------------------------------------------------------
 * Bounded, and it says which lock and who held it.
 * --------------------------------------------------------------------
 *
 * A spin with no bound is a machine that stops with no output, which on a
 * secondary is a core wedged with the boot log already past it. The bound is
 * enormous - far longer than any critical section here can legitimately run
 * - so reaching it means a deadlock rather than contention, and the panic
 * names the lock and the processor that holds it. Nothing else in this
 * kernel could tell you either.
 */

/*
 * Not a valid processor index. `holder` reads this while the lock is free,
 * so a panic can say "held by 2" or "held by nobody", and the second is a
 * different bug from the first.
 */
#define SPIN_NOBODY     ((unsigned)-1)

struct spinlock {
    volatile unsigned locked;
    volatile unsigned holder;

    /* For the panic. A pointer to a literal, so it costs nothing to carry
     * and there is no way for it to be stale. */
    const char *name;

    /*
     * **Where it was last taken**, for the panic too: the function that
     * called `spin_lock`, its name a literal. A holder cannot be asked
     * what it is doing - it holds the lock with interrupts masked, so it
     * would answer after letting go - and "held by 1" alone sent three
     * chases of this panic to guessing (`roadmap.md`, FOUND three times).
     */
    const char *taker;
};

/*
 * Locks are file-scope statics, initialised where they are declared.
 *
 *   static struct spinlock threads_lock = SPINLOCK("threads");
 */
#define SPINLOCK(n)     { 0u, SPIN_NOBODY, (n), NULL }

/*
 * How long a lock may be contended before this is a deadlock rather than a
 * wait. Every critical section in this kernel is a handful of instructions.
 *
 * **Measured, because this said ten million spins was milliseconds even
 * under TCG.** It is about ten milliseconds on AArch64 and a second and a
 * half on x86, where every spin also answers shootdowns - and the most any
 * lock waited in a whole suite run was 2,504 spins on one and 587 on the
 * other, with four x86 guests running at once. The one time this fired in
 * the suite it was not a wait at all: the holder had faulted after taking
 * the lock and before writing its name. `docs/smp.md` has that.
 */
#define SPIN_GIVE_UP    10000000UL

#include "panic.h"

void spin_panic(const struct spinlock *lock);

/*
 * **Patience in time, and not only in spins.** Ten million spins is about
 * ten milliseconds on AArch64 under TCG - less than a Mac running a gate's
 * guests may pause one of them - so a holder whose host thread was paused
 * tripped the panic exactly as a deadlock would, three times
 * (`testing.md` 18.134, 18.287). So a waiter that has spun its ten million
 * asks this, and goes on until a second of the counter has passed since it
 * first asked: a second is a deadlock on any machine and a paused thread on
 * none. `since` starts at zero.
 */
bool spin_patient(uint64_t *since);

/*
 * The wait, when the lock was held: out of line, since it is the uncommon
 * case and everything it needs - a count, a deadline on the stack, a
 * constant of ten million - cost the common case instructions it never used
 * (`spinlock.c`, `testing.md` 18.343).
 */
unsigned long spin_lock_wait(struct spinlock *lock, const char *taker,
                             unsigned long flags);

/*
 * Take the lock, and return what the interrupt state was.
 *
 * The flags go back to `spin_unlock`, which is why they are returned rather
 * than kept in the lock: two cores hold two different previous states, and a
 * field in the lock would be one of them overwriting the other.
 *
 * **One try inline, and the wait out of line** (`testing.md` 18.343). An
 * IPC round trip takes seven locks and finds every one free, and the loop
 * this was - its count, its deadline kept on the stack because
 * `spin_patient` takes its address, its constant - was paid on each of them
 * before the first try.
 */
static inline unsigned long spin_lock_at(struct spinlock *lock, const char *taker)
{
    unsigned long flags = cpu_interrupts_save();

    /*
     * A machine that is halting takes no locks. `panic.h` says why, and it
     * is not a nicety: the panic worth printing most is the one raised
     * inside a console write, and that one owns the console lock already.
     */
    if (panicking()) {
        return flags;
    }

    if (__builtin_expect(!cpu_lock_try(&lock->locked), 0)) {
        return spin_lock_wait(lock, taker, flags);
    }

    lock->holder = this_cpu()->index;
    lock->taker = taker;
    return flags;
}

/* Every caller's name goes with it, for the panic: `taker` above. */
#define spin_lock(lock) spin_lock_at((lock), __func__)

static inline void spin_unlock(struct spinlock *lock, unsigned long flags)
{
    /* Nothing was taken, so there is nothing to give back - and releasing a
     * lock this processor does not hold would be worse than holding it. */
    if (panicking()) {
        cpu_interrupts_restore(flags);
        return;
    }

    lock->holder = SPIN_NOBODY;
    cpu_lock_release(&lock->locked);
    cpu_interrupts_restore(flags);
}

/*
 * Whether this processor is the one holding it.
 *
 * For assertions inside a function that must be called with a lock already
 * held, which is how a lock discipline stays true after the person who wrote
 * it has gone. Never for deciding whether to take it - a lock you have to
 * ask about is a lock you do not hold.
 */
static inline bool spin_held_here(const struct spinlock *lock)
{
    return lock->locked != 0u && lock->holder == this_cpu()->index;
}

#endif /* KERNEL_SPINLOCK_H */
