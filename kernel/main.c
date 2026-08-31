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
#include "cpu.h"
#include "ipc.h"
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
    struct cpu_info cpu;
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
    boot_why("Somewhere to report from. Nothing above this line can say that");
    boot_why("it failed, so it goes first and stays simple: no interrupts, no");
    boot_why("buffering, just a status bit and a data register.");
    boot_fact("PL011 UART at 0x09000000, polled");

    /* Before anything else that could fault. Until this runs, VBAR_EL1 holds
     * whatever the firmware left, and any exception is a jump into nothing. */
    trap_init();
    boot_stage("exception vectors");
    boot_why("Where the processor jumps when something goes wrong. Until VBAR_EL1");
    boot_why("points at real code, a fault jumps into whatever the firmware left");
    boot_why("behind - so a bug before this line is a silent hang, and after it");
    boot_why("is a dump saying which instruction and which address.");
    boot_fact("16 entries at VBAR_EL1, four instructions each");

    /*
     * Who we are running on, asked of the processor.
     *
     * Before the allocator, because it needs nothing: every value comes out
     * of a system register the architecture requires the core to implement.
     * It is also the single most useful line in the log the first time this
     * boots on something that is not QEMU.
     */
    cpu_identify(&cpu);
    boot_stage("processor");
    boot_why("Asking the core who it is. Every AArch64 part must implement these");
    boot_why("registers, so this needs no board knowledge and works anywhere -");
    boot_why("and on the first boot of new hardware it is the line that matters.");

    boot_fact_begin();
    kputs(cpu.implementer_name);
    kputc(' ');
    kputs(cpu.part_name);
    kputs(" r");
    kputu(cpu.variant);
    kputc('p');
    kputu(cpu.revision);
    kputs("  (MIDR_EL1 0x");
    kputx(cpu.midr, 8);
    kputc(')');
    boot_fact_end();

    boot_fact_begin();
    kputu(cpu_pa_bits(&cpu));
    kputs("-bit physical addresses, ");
    kputu(cpu_dcache_line(&cpu));
    kputs("-byte cache lines");
    boot_fact_end();

    boot_fact_begin();
    kputs("counter at ");
    kputu(cpu.counter_hz / 1000000);
    kputs(" MHz - not the core clock; AArch64 has no way to read that");
    boot_fact_end();

    pmm_init();
    boot_stage("physical memory");
    boot_why("A bitmap with one bit per page of RAM: the allocator everything");
    boot_why("else is built on. It has to exist before page tables, because a");
    boot_why("page table is itself made of pages. Its own bitmap goes directly");
    boot_why("after the kernel image and then marks itself used.");

    boot_fact_begin();
    kputu(pmm_total_pages() * (PAGE_SIZE / 1024) / 1024);
    kputs(" MB of RAM in ");
    kputu(pmm_total_pages());
    kputs(" pages of ");
    kputu(PAGE_SIZE / 1024);
    kputs(" KB");
    boot_fact_end();

    boot_fact_begin();
    kputu(pmm_total_pages() - pmm_free_pages());
    kputs(" pages already the kernel's: its image, its bitmap and its stacks");
    boot_fact_end();

    /* Translation on. Everything above ran with the MMU off, where every
     * access is uncached Device memory. From here the kernel runs cached,
     * .text is read-only, and address 0 and the stack guard have no
     * translation at all. */
    mmu_init();
    boot_stage("virtual memory");
    boot_why("Translation on. Everything above ran with the MMU off, where every");
    boot_why("access is uncached and nothing is protected. From here the kernel");
    boot_why("runs cached, its code is read-only even to itself, and the pages");
    boot_why("that should not be touched have no translation at all - so a null");
    boot_why("dereference and a stack overflow both become faults with addresses");
    boot_why("rather than silent corruption.");
    boot_fact("4 KB granule, 39-bit addresses, TTBR0");
    boot_fact(".text read-only, .rodata never executable, page 0 unmapped");
    boot_fact("a guard page under each stack, deliberately absent from the map");

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
    boot_why("Asking the firmware for a linear framebuffer. Under QEMU that is");
    boot_why("ramfb, reached through fw_cfg: the guest hands over an address, a");
    boot_why("format and a size, and the emulator scans that memory out. The Pi");
    boot_why("mailbox is the same conversation with a different messenger, which");
    boot_why("is why the HAL call is shaped this way.");

    if (have_display) {
        boot_fact_begin();
        kputu(fb.width);
        kputc('x');
        kputu(fb.height);
        kputs(", 32-bit XRGB");
        boot_fact_end();

        boot_fact_begin();
        kputu(fb.pitch);
        kputs(" bytes a row, not ");
        kputu(fb.width * 4);
        kputs(" - the stride is padded on purpose, so that code");
        boot_fact_end();

        boot_fact("   assuming width * 4 shears here rather than on real hardware");
    } else {
        boot_fact("none attached; the serial line is the only console");
    }

    boot_stage("keyboard");
    boot_why("This machine has no PS/2 port, so a real key comes over virtio.");
    boot_why("Thirty-two fixed windows in the memory map, each with a magic");
    boot_why("number and a device id at the top of it: discovery is reading two");
    boot_why("registers thirty-two times. No PCI bus to walk, which is what makes");
    boot_why("this a short driver - and the same transport gives the GPU next.");

    if (hal_keyboard_init()) {
        boot_fact("virtio-input found, negotiated and polled like the serial line");
    } else {
        boot_fact("none found; input comes over the serial line");
    }

    thread_init();
    boot_stage("threads");
    boot_why("A fixed pool, because the kernel has no allocator: running out is");
    boot_why("an error at a known limit rather than a failure at an unknown one.");
    boot_why("Each thread owns two stacks - one for its own code and one the");
    boot_why("processor switches to on an exception - because a fault caused by");
    boot_why("an exhausted stack must not build its report on that same stack.");

    boot_fact_begin();
    kputu(THREAD_MAX);
    kputs(" slots, two stacks each, a guard page below every one");
    boot_fact_end();

    ipc_init();
    boot_stage("IPC and capabilities");
    boot_why("How processes talk, and the only way they can. A send blocks until");
    boot_why("a receiver takes it, so the kernel buffers nothing and there is no");
    boot_why("queue to size or overflow. What a process may reach is whatever is");
    boot_why("in its own capability table - an index, never a global name, so");
    boot_why("there is nothing to guess and nothing to enumerate.");

    boot_fact_begin();
    kputu(ENDPOINT_MAX);
    kputs(" endpoints, ");
    kputu(CAPS_PER_THREAD);
    kputs(" capabilities a thread, generation-numbered against reuse");
    boot_fact_end();

    process_init();
    boot_stage("processes");
    boot_why("A process is an address space plus a thread at EL0. The kernel is");
    boot_why("mapped into every one of them and reachable from none: the page");
    boot_why("table entries say EL1-only, so a process reading kernel memory");
    boot_why("takes a permission fault rather than finding nothing there.");

    boot_fact_begin();
    kputu(PROCESS_MAX);
    kputs(" slots; each gets its own page tables, heap and stack");
    boot_fact_end();

    hal_irq_init();
    hal_timer_init(TICK_HZ);

    /* Nothing has been able to interrupt this core since start.S masked
     * everything on the way into EL1. Now there is a handler and a source. */
    __asm__ volatile("msr daifclr, #2" ::: "memory");

    boot_stage("timer and interrupts");
    boot_why("The heartbeat. Until now a thread that never yielded would run for");
    boot_why("ever; from here the timer interrupts the core and the scheduler");
    boot_why("gets to change its mind. The switch happens in the exception");
    boot_why("vector's last instructions rather than in C, because the frame");
    boot_why("being restored has to belong to the thread about to resume.");

    boot_fact_begin();
    kputu(TICK_HZ);
    kputs(" Hz off the generic timer, delivered through a GICv3");
    boot_fact_end();

    boot_fact_begin();
    kputs("scheduler: ");
    kputs(sched_current()->name);
    kputs(" - the policy is behind an interface, so another is a new file");
    boot_fact_end();

    boot_stage("userland");
    boot_why("The kernel's job ends here. It starts one process and stops: init");
    boot_why("creates the console server, the filesystem and the shell, and hands");
    boot_why("each exactly the capabilities it needs. Nothing below this line");
    boot_why("knows what a file is, what a window is, or what Lua is.");

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
    boot_fact("the test suite, which stands in for init in this image");
    tests_run();        /* never returns: exits the guest through semihosting */
#endif

#ifdef KOSMOS_BENCH
    boot_fact("the benchmarks, which stand in for init in this image");
    bench_run();        /* likewise */
#endif

    boot_fact("init: 1 process, which will become 4");

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
        cap_t devices_ep = ipc_endpoint_create();

        if (console_ep < 0 || ramfs_ep < 0 || devices_ep < 0) {
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

        /* At the indices init expects them, and checked rather than assumed:
         * a capability that lands one slot over is a server talking to the
         * wrong endpoint, which fails in a way that looks like a protocol
         * bug rather than a wiring one. */
        if (ipc_cap_grant(init->thread, console_ep) != 0
            || ipc_cap_grant(init->thread, ramfs_ep) != 1
            || ipc_cap_grant(init->thread, devices_ep) != 2) {
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
    /* Named as the idle thread, so that every timer tick can tell whether
     * the machine was working or waiting. It is the only thread that runs
     * when there is nothing to do, which is what makes the answer honest. */
    thread_set_idle(thread_current());

    for (;;) {
        thread_yield();
        __asm__ volatile("wfi");
    }
}
