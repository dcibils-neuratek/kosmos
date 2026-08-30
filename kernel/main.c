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
     * Two Lua processes, at EL0, in separate address spaces, exchanging a
     * Lua table over the microkernel's IPC. That is M4's definition of done.
     *
     * The kernel creates the endpoint and hands each of them a capability
     * for it. Neither can name it any other way, and neither can reach
     * anything else: what a process has is what it was given, which is
     * design.md 4.3 with nothing else left to appeal to.
     */
    {
        extern const unsigned char init_image[];
        extern const unsigned long init_image_len;
        struct process *server;
        struct process *client;
        cap_t ep = ipc_endpoint_create();
        unsigned i;

        if (ep < 0) {
            panic("no endpoint for init");
        }

        server = process_create("echo", init_image,
                                (size_t)init_image_len, 1 /* server */);
        client = process_create("init", init_image,
                                (size_t)init_image_len, 0 /* client */);

        if (server == NULL || client == NULL) {
            kputs("could not create the init processes\n");
        } else {
            /*
             * Each gets its own index for the same endpoint, and each gets
             * it as capability zero because its table was empty. That is
             * the whole convention: a process's first capability is what it
             * was started for.
             */
            if (ipc_cap_grant(server->thread, ep) != 0
                || ipc_cap_grant(client->thread, ep) != 0) {
                panic("init capabilities did not land at index 0");
            }

            process_start(server);
            process_start(client);

            for (i = 0; i < 8192 && !client->exited; i++) {
                thread_yield();
            }

            (void)ipc_endpoint_destroy(ep);
        }
    }

    {
        struct lua_State *L = kosmos_lua_open();

        if (L == NULL) {
            panic("Lua could not start: the heap was too small");
        }

        kputs("lua   ");
        kputs(kosmos_lua_version());
        kputc('\n');

        /* Never returns. Everything from here is typed at the prompt. */
        repl_run(L);
    }
}
