/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What a lock says when it gives up.
 *
 * Out of line because `spinlock.h` is included by half the kernel and this
 * pulls in the console, and because the one thing a deadlock must not do is
 * make the code that takes the lock any bigger.
 */

#include "hal.h"
#include "panic.h"
#include "spinlock.h"

/*
 * Straight to the serial line, past the console.
 *
 * **`kputs` takes the console's lock, and this is a function about locks
 * that will not be taken.** If the lock that deadlocked *is* the console's,
 * reporting it through `kputs` spins on the same lock, panics again, and
 * recurses until the stack runs out - turning a diagnosable deadlock into a
 * silent triple fault.
 *
 * So this writes bytes to the UART and nothing else: no log buffer, no
 * screen, no lock. It loses the report from the framebuffer, which is the
 * right trade for a message that only appears when the machine is already
 * broken and whose whole value is being readable.
 */
static void raw(const char *s)
{
    for (; *s != '\0'; s++) {
        if (*s == '\n') {
            hal_putchar('\r');
        }

        hal_putchar(*s);
    }
}

static void raw_num(unsigned long v)
{
    char     buf[24];
    unsigned i = 0;

    if (v == 0) {
        hal_putchar('0');
        return;
    }

    while (v > 0) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }

    while (i > 0) {
        hal_putchar(buf[--i]);
    }
}

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
    raw("spinlock: ");
    raw(lock->name ? lock->name : "(unnamed)");
    raw(" held by ");

    if (lock->holder == SPIN_NOBODY) {
        raw("nobody");
    } else {
        raw_num(lock->holder);
    }

    raw(", wanted by ");
    raw_num(this_cpu()->index);
    raw("\n");

    panic("spinlock: gave up waiting");
}
