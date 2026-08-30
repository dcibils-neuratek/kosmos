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
#include "panic.h"

#ifdef KOSMOS_TEST
#include "test.h"
#endif

#ifdef KOSMOS_BENCH
#include "bench.h"
#endif

/*
 * The boot splash.
 *
 * The display's counterpart to the "mem 512 MB" line above: proof at every
 * boot that the device came up, in a form a person can check at a glance.
 * All real drawing moves to userland at the next step, and this stays,
 * because a system that prints its RAM size and says nothing about its
 * screen is harder to debug than one that does both.
 *
 * Deliberately shaped so that the two ways this can be subtly wrong are
 * obvious rather than plausible:
 *
 *   - **A two-pixel white border.** If the pitch is wrong - if anything
 *     computed an offset as width * 4 instead of using it - the top and
 *     bottom edges still look right and the left and right ones shear into
 *     diagonals. Nothing else in a test pattern shows that as clearly.
 *   - **Red, green and blue bars, in that order, left to right.** If the
 *     channel order or the fourcc is wrong they come out reversed, and a
 *     blue-first pattern is unmistakable where a slightly-off colour is not.
 *
 * The row pointer is recomputed from `pitch` each line, which is the same
 * discipline `gfx.md` §19.3 demands of every primitive that comes after it.
 */
static void splash(const struct fb *fb)
{
    unsigned y;

    for (y = 0; y < fb->height; y++) {
        volatile uint32_t *row =
            (volatile uint32_t *)((volatile uint8_t *)fb->pixels
                                  + (size_t)y * fb->pitch);
        unsigned x;

        for (x = 0; x < fb->width; x++) {
            uint32_t colour;

            if (x < 2 || y < 2 || x + 2 >= fb->width || y + 2 >= fb->height) {
                colour = 0x00ffffffu;                   /* the border */
            } else if (y >= fb->height / 3 && y < (fb->height * 2) / 3) {
                unsigned third = fb->width / 3;

                if (x < third) {
                    colour = 0x00c03030u;               /* red   */
                } else if (x < third * 2) {
                    colour = 0x0030c030u;               /* green */
                } else {
                    colour = 0x003030c0u;               /* blue  */
                }
            } else {
                /* A vertical ramp, so a row written to the wrong place
                 * breaks the gradient visibly. */
                uint32_t v = (y * 255u) / fb->height;
                colour = (v / 3) << 16 | (v / 3) << 8 | v;
            }

            row[x] = colour;
        }
    }
}

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
     * No heap.
     *
     * There was a 2 MB one here, taken from the page allocator for a
     * `lua_State` the kernel opened. Neither exists any more: the kernel's
     * own state lives in fixed pools, and nothing in kernel/, arch/ or hal/
     * calls malloc. A process gets its heap mapped into its own address
     * space, which is where a heap that can be exhausted belongs.
     */
    thread_init();
    ipc_init();
    process_init();
    kputs("sched ");
    kputs(sched_current()->name);
    kputc('\n');

    /*
     * The display, if there is one.
     *
     * False is not an error. `make test` boots without `-device ramfb` and
     * the Pi has no display attached to it yet, and a system that cannot
     * come up without a screen is a system that cannot be debugged over a
     * serial cable.
     */
    {
        struct fb fb;

        if (hal_fb_init(&fb)) {
            splash(&fb);

            /* Remembered, because a process that is granted the screen needs
             * these pages mapped and hal_fb_init cannot be asked again: it
             * clears the framebuffer. */
            screen_init(&fb);

            kputs("video ");
            kputu(fb.width);
            kputc('x');
            kputu(fb.height);
            kputs(", pitch ");
            kputu(fb.pitch);
            kputs(" bytes, at 0x");
            kputx((unsigned long)(uintptr_t)fb.pixels, 16);
            kputc('\n');
        } else {
            kputs("video none, serial only\n");
        }
    }

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
