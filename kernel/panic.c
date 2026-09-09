/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#include "cpu.h"
#include "panic.h"
#include "console.h"

/*
 * Set once, never cleared: there is no coming back from here, and a second
 * core arriving in `panic` must find it already true rather than race to
 * set it.
 */
static volatile bool in_panic;

bool panicking(void)
{
    return in_panic;
}

void panic(const char *msg)
{
    /*
     * **Before anything that could take a lock**, which is everything
     * below. `panic.h` has the account: a fault inside a console write
     * holds the console lock, and a panic that waits for it deadlocks
     * against its own caller.
     */
    in_panic = true;

    /*
     * The screen back, whatever had it.
     *
     * A compositor can take the screen from this console, and while it has
     * it a panic would be printed to the serial line and nowhere else - so a
     * machine with a window manager running and no cable attached would stop
     * dead with a desktop on it and no explanation anywhere. Whatever was
     * being drawn matters less than the reason the machine stopped.
     */
    console_screen_resume();

    kputs("\nPANIC: ");
    kputs(msg);
    kputs("\n");

    for (;;) {
        cpu_wait_for_interrupt();
    }
}
