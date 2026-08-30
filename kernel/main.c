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
     * A ramfs server and two clients, each a process at EL0 in its own
     * address space, talking the namespace protocol.
     *
     * The kernel plays init here, which is not where it belongs: `roadmap.md`
     * has init and supervision in userland, and that needs a process able to
     * start processes. Until there is a spawn syscall, the wiring is done
     * from here, and it is the only part of this that is temporary.
     *
     * What is not temporary is the shape. Each process is handed exactly one
     * capability and can name nothing else, and the two clients mount the
     * same server under different names, which is the milestone's point:
     * what a process has not mounted does not exist.
     */
    {
        extern const unsigned char init_image[];
        extern const unsigned long init_image_len;
        size_t image_len = (size_t)init_image_len;
        struct process *ramfs;
        struct process *client_a;
        struct process *client_b;
        cap_t ep = ipc_endpoint_create();
        unsigned i;

        if (ep < 0) {
            panic("no endpoint for the ramfs");
        }

        ramfs    = process_create("ramfs", init_image, image_len, 1);
        client_a = process_create("client-a", init_image, image_len, 0);
        client_b = process_create("client-b", init_image, image_len, 2);

        if (ramfs == NULL || client_a == NULL || client_b == NULL) {
            kputs("could not create the M5 processes\n");
        } else {
            /* One capability each, granted before any of them can run. Each
             * gets its own index for the same endpoint; the number means
             * nothing outside the table it came from. */
            if (ipc_cap_grant(ramfs->thread, ep) != 0
                || ipc_cap_grant(client_a->thread, ep) != 0
                || ipc_cap_grant(client_b->thread, ep) != 0) {
                panic("capabilities did not land at index 0");
            }

            process_start(ramfs);
            process_start(client_a);
            process_start(client_b);

            for (i = 0; i < 16384 && !(client_a->exited && client_b->exited); i++) {
                thread_yield();
            }

            /* Destroying the endpoint is what stops the server: its receive
             * fails and it leaves its loop. */
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
