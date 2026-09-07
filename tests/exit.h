/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef TESTS_EXIT_H
#define TESTS_EXIT_H

/*
 * How the suite tells the host whether it passed.
 *
 * `run_tests.py` requires three signals to agree - the boot banner, a
 * complete TAP stream, and QEMU's own exit code - because any one of them
 * alone is too easy to pass by accident. A guest that faults before
 * printing produces no TAP; a guest that prints a flawless plan and then
 * hangs never sets an exit code. This header is the third of those, and it
 * is the one thing in the suite that is different on each board.
 *
 * **AArch64 has semihosting**, which is a debug channel the guest can call
 * into with a request number, and QEMU implements SYS_EXIT by exiting with
 * the status it is handed. One instruction, an exact status, and the same
 * mechanism a debugger on real hardware would offer.
 *
 * **x86-64 has nothing of the kind**, and what it has instead is a pair of
 * asymmetric doors:
 *
 *   - **Success is a power-off.** `hal_power_off` writes S5 to q35's ACPI
 *     PM1a block and QEMU exits 0, which is the code we actually want and
 *     the one a normal shutdown produces anyway.
 *
 *   - **Failure is `isa-debug-exit`.** QEMU's debug port exits with
 *     `(value << 1) | 1`, which is always odd and *can never be zero* - so
 *     it cannot express success at all, and using it for both would mean
 *     the runner treating 1 as "passed", which is a trap laid for whoever
 *     reads the harness next. Writing 1 here exits 3.
 *
 * The asymmetry is the honest shape rather than a workaround: two different
 * things happened, and on this machine they leave by two different doors.
 * It needs `-device isa-debug-exit,iobase=0xf4,iosize=0x04` on the QEMU
 * line, and without it a failing run halts instead of exiting - which the
 * runner's timeout catches and reports as a hang, not as a pass.
 */

#if defined(__aarch64__)

#include "semihosting.h"

static inline void tests_exit(int code)
{
    semihosting_exit(code);
}

#elif defined(__x86_64__)

#include "hal.h"

/* QEMU's debug exit device, at the iobase the runner puts on the line. */
#define ISA_DEBUG_EXIT_PORT     0xf4

/* Any non-zero value; QEMU turns it into `(value << 1) | 1`, so this is
 * exit code 3. The number itself means nothing beyond "not a success", and
 * the runner is told to expect this one. */
#define ISA_DEBUG_EXIT_FAILED   1

static inline void tests_exit(int code)
{
    if (code == 0) {
        hal_power_off();        /* never returns; QEMU exits 0 */
    }

    __asm__ volatile("outl %0, %1"
                     :: "a"((unsigned)ISA_DEBUG_EXIT_FAILED),
                        "Nd"((unsigned short)ISA_DEBUG_EXIT_PORT));

    /* Only reached if the device is not on the QEMU line. Halting rather
     * than returning: the suite is finished either way, and a runner that
     * times out here says "it hung", which is true and is better than
     * carrying on into a kernel that has already printed its results. */
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

#else
#error "no way to exit the guest on this architecture"
#endif

#endif /* TESTS_EXIT_H */
