/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * panic(), from inside a process.
 *
 * Same interface as the kernel's and a different meaning, which is the
 * clearest single illustration of what an address space bought. In the
 * kernel a panic stops the machine because there is nothing else to stop. In
 * here it ends one process and the system carries on.
 */

#include <string.h>

#include "panic.h"
#include "kosmos.h"

/* The POSIX layer's road to the console, in an image that links it
 * (`runtime/posix/fd.c`): a process that is not the console's owner cannot
 * `kosmos_write`, and its panic was silent. */
void posix_last_words(const char *s, unsigned long bytes) __attribute__((weak));

void panic_say(const char *s, unsigned long bytes)
{
    if (posix_last_words != 0) {
        posix_last_words(s, bytes);
    } else {
        (void)kosmos_write(s, bytes);
    }
}

void panic(const char *msg)
{
    panic_say("panic: ", 7);
    panic_say(msg, strlen(msg));
    panic_say("\n", 1);

    kosmos_exit(70);
}
