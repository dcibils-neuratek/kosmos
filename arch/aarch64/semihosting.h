/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef ARCH_AARCH64_SEMIHOSTING_H
#define ARCH_AARCH64_SEMIHOSTING_H

#include <stdint.h>

/*
 * Semihosting: how the guest talks to whatever is running it.
 *
 * Only one call is used, and only by the test build: SYS_EXIT, so the guest
 * can set QEMU's process exit code. Without it the host runner has to kill
 * QEMU on a timeout and guess the result from the output, which is both slow
 * and wrong whenever the guest dies in a way that still printed the right
 * text.
 *
 * Requires `-semihosting-config enable=on,target=native` on the QEMU line.
 * If semihosting is not enabled, `hlt #0xf000` is an undefined instruction
 * trap: with no exception vector installed that is a silent hang, which is
 * why this parks in a loop afterwards rather than claiming to be
 * unreachable.
 *
 * Reference: ARM "Semihosting for AArch32 and AArch64" (ARM DUI 0203 /
 * semihosting spec), SYS_EXIT.
 */

/*
 * **`SEMIHOSTING_EXIT` rather than the spec's `SYS_EXIT`**, because this
 * system has a syscall of that name and it is number 0, not 0x18.
 *
 * Two unrelated numbering schemes both call their first operation SYS_EXIT:
 * ARM's semihosting interface, which is what this header speaks, and
 * Kosmos's own, in `kernel/syscall.h`. Nothing included both until a test
 * wanted a constant from the second one, at which point the collision is a
 * `-Werror` redefinition and the compiler names the wrong file first.
 *
 * The spec's name is kept in the comments, where it points somebody at the
 * document; the identifier is prefixed, where it has to be unique.
 */
#define SEMIHOSTING_EXIT                0x18

/* The guest is stopping because the application finished normally, as
 * opposed to a breakpoint or a fault. This is the reason code that makes
 * QEMU exit with our status rather than its own. */
#define ADP_STOPPED_APPLICATION_EXIT    0x20026UL

static inline void semihosting_exit(int code)
{
    /*
     * On AArch64 this call does not take the status in a register. x1 points
     * at a two-field block: the reason, then the exit status. Passing the
     * status directly is the AArch32 form and silently exits 0 here.
     */
    volatile uint64_t block[2] = {
        ADP_STOPPED_APPLICATION_EXIT,
        (uint64_t)(unsigned int)code,
    };

    register uint64_t x0 __asm__("x0") = SEMIHOSTING_EXIT;
    register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)block;

    __asm__ volatile("hlt #0xf000" : : "r"(x0), "r"(x1) : "memory");

    /* Only reached if semihosting is off and the trap was swallowed. */
    for (;;) {
        __asm__ volatile("wfi");
    }
}

#endif /* ARCH_AARCH64_SEMIHOSTING_H */
