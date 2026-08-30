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

void panic(const char *msg)
{
    (void)kosmos_write("panic: ", 7);
    (void)kosmos_write(msg, strlen(msg));
    (void)kosmos_write("\n", 1);

    kosmos_exit(70);
}
