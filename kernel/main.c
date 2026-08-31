/*
 * Where C starts. Called from boot/start.S with the stack set up and .bss
 * zeroed, running at EL1 on core 0.
 */

#include <stddef.h>
#include <stdint.h>

#include "hal.h"
#include "console.h"
#include "trap.h"
#include "pmm.h"
#include "page.h"
#include "mmu.h"
#include "kernel.h"

#include "thread.h"
#include "sched.h"
#include "ipc.h"
#include "process.h"
#include "screen.h"
#include "boot.h"
#include "panic.h"

#ifdef KOSMOS_TEST
#include "test.h"
#endif

#ifdef KOSMOS_BENCH
#include "bench.h"
#endif

void kmain(void)
{
    struct fb fb;
    bool have_display;

    /*
     * The boot, out loud.
     *
     * Ten stages, each announced before it happens and each followed by the
     * numbers that make it worth watching. This is a system built to be
     * learned from, and almost everything interesting about it happens
     * between the reset vector and the prompt - a boot that prints four
     * lines and drops you at a shell has hidden the entire subject.
     *
     * The order below is not arrangeable. Each stage needs the one above it:
     * there is no output before the UART, no fault report before the vector
     * table, no page tables before an allocator to build them from, and no
     * process before there are threads to run it on.
     */
    hal_early_init();
    kputs("\nKosmos\n\n");
    boot_stage("serial port");
    boot_detail("PL011 at 0x09000000, polled; the only way out until M6");

    /* Before anything else that could fault. Until this runs, VBAR_EL1 holds
     * whatever the firmware left, and any exception is a jump into nothing. */
    trap_init();
    boot_stage("exception vectors");
    boot_detail("16 entries at VBAR_EL1; a fault now reports itself");

    pmm_init();
    boot_stage("physical memory");
    kputs("       ");
    kputu(pmm_total_pages() * (PAGE_SIZE / 1024) / 1024);
    kputs(" MB in ");
    kputu(pmm_total_pages());
    kputs(" pages of 4 KB, ");
    kputu(pmm_total_pages() - pmm_free_pages());
    kputs(" already the kernel's\n");

    /* Translation on. Everything above ran with the MMU off, where every
     * access is uncached Device memory. From here the kernel runs cached,
     * .text is read-only, and address 0 and the stack guard have no
     * translation at all. */
    mmu_init();
    boot_stage("virtual memory");
    boot_detail("4 KB pages, 39-bit addresses; .text read-only, page 0 unmapped");

    /*
     * The display, if there is one.
     *
     * False is not an error. `make test` boots without `-device ramfb` and
     * a board may have no monitor attached, and a system that cannot come up
     * without a screen is a system that cannot be debugged over a cable.
     */
    have_display = hal_fb_init(&fb);

    if (have_display) {
        /* Remembered, because a process granted the screen needs these pages
         * mapped and hal_fb_init cannot be asked again: it clears the
         * framebuffer. */
        screen_init(&fb);

        /*
         * And from here the kernel's own output goes to both. Everything
         * already printed above went to the serial port alone, which is why
         * the screen starts at this line rather than at the first - the
         * alternative is buffering the boot log to replay it, and a buffer
         * that exists only to make the start look tidy is a buffer that can
         * overflow during a panic.
         */
        console_attach_screen(&fb, "Kosmos");
        console_progress(boot_stages_done(), BOOT_STAGES);
    }

    boot_stage("display");

    if (have_display) {
        kputs("       ");
        kputu(fb.width);
        kputc('x');
        kputu(fb.height);
        kputs(", 32-bit colour, ");
        kputu(fb.pitch);
        kputs(" bytes a row (not ");
        kputu(fb.width * 4);
        kputs(": the stride is padded)\n");
    } else {
        boot_detail("none; serial only");
    }

    thread_init();
    boot_stage("threads");
    boot_detail("a fixed pool, each with two stacks and a guard page below");

    ipc_init();
    boot_stage("IPC and capabilities");
    boot_detail("synchronous rendezvous; a capability is an index, never a name");

    process_init();
    boot_stage("processes");
    boot_detail("address spaces at EL0; the kernel is mapped and untouchable");

    hal_irq_init();
    hal_timer_init(TICK_HZ);

    /* Nothing has been able to interrupt this core since start.S masked
     * everything on the way into EL1. Now there is a handler and a source. */
    __asm__ volatile("msr daifclr, #2" ::: "memory");

    boot_stage("timer and interrupts");
    kputs("       ");
    kputu(TICK_HZ);
    kputs(" Hz, GICv3; preemption starts here, in the vector's epilogue\n");

    kputs("       scheduler: ");
    kputs(sched_current()->name);
    kputc('\n');

    boot_stage("userland");

    /*
     * In the test and bench images, the suite *is* the userland: it runs
     * here instead of init and never comes back.
     *
     * These two calls were silently deleted by a careless rewrite of this
     * function, and the symptom was not a failure - it was `make test`
     * apparently hanging, because the image booted perfectly into a shell
     * while the host runner sat waiting for a TAP plan that was never
     * coming. The second time a slice-based edit has quietly dropped lines
     * from the middle of something.
     */
#ifdef KOSMOS_TEST
    boot_detail("the test suite, which stands in for init in this image");
    tests_run();        /* never returns: exits the guest through semihosting */
#endif

#ifdef KOSMOS_BENCH
    boot_detail("the benchmarks, which stand in for init in this image");
    bench_run();        /* likewise */
#endif

    boot_detail("init, which starts everything else and outlives it");

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

        /*
         * And the screen, for the same reason and with the same rule: a
         * spawn refuses to hand on a device the parent does not hold, so
         * init cannot give the shell a screen it was never given itself.
         *
         * Ignored when it fails. A machine booted without a display is a
         * supported way to run, and init's job is to start the system rather
         * than to insist on a monitor.
         */
        (void)process_grant_screen(init);

        if (ipc_cap_grant(init->thread, console_ep) != 0
            || ipc_cap_grant(init->thread, ramfs_ep) != 1) {
            panic("init's capabilities did not land where expected");
        }

        process_start(init);
    }

    /*
     * Nothing left for this thread to do but keep the scheduler company.
     *
     * There is no Lua behind this line any more, and no REPL. Both were in
     * the image long after the prompt moved into a process, kept there by a
     * test suite that drove the kernel through an interpreter; the tests run
     * at EL0 now and the interpreter went with them.
     */
    for (;;) {
        thread_yield();
        __asm__ volatile("wfi");
    }
}
