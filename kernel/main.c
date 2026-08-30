/*
 * Where C starts. Called from boot/start.S with the stack set up and .bss
 * zeroed, running at EL1 on core 0.
 */

#include "hal.h"

static void kputs(const char *s)
{
    for (; *s != '\0'; s++) {
        /* A serial terminal needs both, or the next line starts wherever
         * this one ended and the log becomes a staircase. */
        if (*s == '\n') {
            hal_putchar('\r');
        }
        hal_putchar(*s);
    }
}

void kmain(void)
{
    hal_early_init();
    kputs("Kosmos\n");

    /* wfi rather than a busy loop: it parks the core until an interrupt
     * arrives instead of pinning a host CPU at 100% under QEMU. Nothing can
     * wake it yet, which is exactly right for M0. */
    for (;;) {
        __asm__ volatile("wfi");
    }
}
