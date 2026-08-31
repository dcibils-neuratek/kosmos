#include "panic.h"
#include "console.h"

void panic(const char *msg)
{
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
        __asm__ volatile("wfi");
    }
}
