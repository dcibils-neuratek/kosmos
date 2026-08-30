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

#include <stdlib.h>
#include "kosmos_lua.h"
#include "thread.h"
#include "sched.h"
#include "ipc.h"
#include "process.h"
#include "panic.h"

#ifdef KOSMOS_TEST
#include "test.h"
#endif

#ifdef KOSMOS_BENCH
#include "bench.h"
#endif

void kmain(void)
{

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

    /*
     * Lua's heap. One contiguous span taken from the page allocator once and
     * sub-divided inside itself, which is what design.md 5.2 means by a
     * lua_State with a heap limit. The kernel's own state stays in fixed
     * pools; nothing in kernel/, arch/ or hal/ calls malloc.
     */
    {
        void *heap = pmm_alloc_contiguous(LUA_HEAP_PAGES);

        if (heap == NULL) {
            panic("no room for the Lua heap");
        }

        heap_init(heap, LUA_HEAP_PAGES * PAGE_SIZE);

        kputs("heap  ");
        kputu(heap_size() / 1024);
        kputs(" KB at 0x");
        kputx((unsigned long)(unsigned long)heap, 16);
        kputs("\n");
    }

    thread_init();
    ipc_init();
    process_init();
    kputs("sched ");
    kputs(sched_current()->name);
    kputc('\n');

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

#ifdef KOSMOS_BENCH
    bench_run();   /* likewise */
#endif

    /* wfi rather than a busy loop: it parks the core until an interrupt
     * arrives instead of pinning a host CPU at 100% under QEMU. Nothing can
     * wake it yet, which is exactly right for M0. */
    /*
     * Start init, and nothing else.
     *
     * The kernel used to create the console server, the ramfs and the shell,
     * and wire their capabilities together. That was init's job being done
     * in the wrong place, and `roadmap.md` says so. Now the kernel makes one
     * process, hands it the console and an endpoint per server it is
     * expected to start, and stops.
     *
     * What init can do is bounded by what it was given. It cannot promote a
     * child beyond itself: a spawn resolves every capability against the
     * parent's own table, and passing on the console is refused unless the
     * parent holds it.
     */
    {
        extern const unsigned char init_image[];
        extern const unsigned long init_image_len;
        struct process *init;
        cap_t console_ep = ipc_endpoint_create();
        cap_t ramfs_ep = ipc_endpoint_create();

        if (console_ep < 0 || ramfs_ep < 0) {
            panic("no endpoints for init");
        }

        init = process_create("init", init_image,
                              (size_t)init_image_len, 7 /* init */);

        if (init == NULL) {
            panic("could not create init");
        }

        /* It holds the console so it can pass it on, and an endpoint for
         * each server, at the indices it expects them. */
        process_grant_console(init);

        if (ipc_cap_grant(init->thread, console_ep) != 0
            || ipc_cap_grant(init->thread, ramfs_ep) != 1) {
            panic("init's capabilities did not land where expected");
        }

        process_start(init);
    }

    /*
     * Nothing left for this thread to do but keep the scheduler company.
     *
     * The kernel's own Lua and its REPL are gone from the boot path: the
     * prompt lives in a process now. Lua is still compiled into the kernel
     * because the test suite drives the kernel through it, and taking it out
     * entirely means moving those tests into userland, which is its own
     * piece of work rather than a line to delete here.
     */
    for (;;) {
        thread_yield();
        __asm__ volatile("wfi");
    }
}
