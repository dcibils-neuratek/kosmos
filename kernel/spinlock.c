/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What a lock says when it gives up.
 *
 * Out of line because `spinlock.h` is included by half the kernel and this
 * pulls in the console, and because the one thing a deadlock must not do is
 * make the code that takes the lock any bigger.
 */

#include "console.h"
#include "panic.h"
#include "spinlock.h"

void spin_panic(const struct spinlock *lock)
{
    /*
     * The two numbers before the panic, because they are different failures
     * and `panic` takes a sentence rather than a format.
     *
     * A lock held by *another* processor for ten million spins is a critical
     * section that never ended - go and look at that core. A lock held by
     * *this* one is a path that took it twice, which is a bug in a single
     * file rather than a race, and is the failure a one-core machine can
     * also reach. Without these two numbers a deadlock panic says only that
     * something stopped.
     */
    kputs("spinlock: ");
    kputs(lock->name ? lock->name : "(unnamed)");
    kputs(" held by ");

    if (lock->holder == SPIN_NOBODY) {
        kputs("nobody");
    } else {
        kputu(lock->holder);
    }

    kputs(", wanted by ");
    kputu(this_cpu()->index);
    kputs("\n");

    panic("spinlock: gave up waiting");
}
