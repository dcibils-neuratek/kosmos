/*
 * Where C starts. Called from boot/start.S with the stack set up and .bss
 * zeroed, running at EL1 on core 0.
 */

#include "hal.h"
#include "console.h"
#include "trap.h"
#include "pmm.h"
#include "page.h"
#include "mmu.h"
#include "kernel.h"

#ifdef KOSMOS_TEST
#include "test.h"
#endif

void kmain(void)
{
    unsigned long last_second = 0;

    hal_early_init();

    /* Before anything else that could fault. Until this runs, VBAR_EL1 holds
     * whatever the firmware left, and any exception is a jump into nothing. */
    trap_init();

    /* The boot banner is itself part of M0's definition of done, and the
     * host runner checks for it. Printed before the tests so that a suite
     * that dies halfway still proves the UART came up. */
    pmm_init();

    kputs("Kosmos\n");

    /* The one line worth printing at every boot. A wrong RAM size or a
     * bitmap that swallowed the wrong range shows up here as a number that
     * looks off, rather than as a fault somewhere else much later. */
    kputs("mem   ");
    kputu(pmm_total_pages() * (PAGE_SIZE / 1024) / 1024);
    kputs(" MB, ");
    kputu(pmm_total_pages());
    kputs(" pages, ");
    kputu(pmm_free_pages());
    kputs(" free, ");
    kputu(pmm_total_pages() - pmm_free_pages());
    kputs(" used by the kernel\n");

    /* Translation on. Everything above ran with the MMU off, where every
     * access is uncached Device memory. From here the kernel runs cached,
     * .text is read-only, and address 0 and the stack guard have no
     * translation at all. */
    mmu_init();
    kputs("mmu   on\n");

    hal_irq_init();
    hal_timer_init(TICK_HZ);

    /* Nothing has been able to interrupt this core since start.S masked
     * everything on the way into EL1. Now there is a handler and a source. */
    __asm__ volatile("msr daifclr, #2" ::: "memory");
    kputs("timer ");
    kputu(TICK_HZ);
    kputs(" Hz\n");

#ifdef KOSMOS_TEST
    tests_run();   /* never returns: exits the guest through semihosting */
#endif

    /* wfi rather than a busy loop: it parks the core until an interrupt
     * arrives instead of pinning a host CPU at 100% under QEMU. Nothing can
     * wake it yet, which is exactly right for M0. */
    for (;;) {
        unsigned long second;

        __asm__ volatile("wfi");

        second = hal_ticks() / TICK_HZ;
        if (second != last_second) {
            last_second = second;
            kputs("tick  ");
            kputu(second);
            kputs("\n");
        }
    }
}
