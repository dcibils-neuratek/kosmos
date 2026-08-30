#include "panic.h"
#include "console.h"

void panic(const char *msg)
{
    kputs("\nPANIC: ");
    kputs(msg);
    kputs("\n");

    for (;;) {
        __asm__ volatile("wfi");
    }
}
