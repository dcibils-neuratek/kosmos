/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#include <stdint.h>

#include "gdt.h"
#include "user.h"

void syscall_entry(void);       /* in user.S */

#define MSR_EFER    0xC0000080u
#define MSR_STAR    0xC0000081u
#define MSR_LSTAR   0xC0000082u
#define MSR_FMASK   0xC0000084u

#define EFER_SCE    (1UL << 0)

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;

    /* EDX:EAX, two 32-bit halves, always - even in long mode. Asking for
     * the pair with `"=A"` reads half the register here and looks like it
     * worked, which is a mistake this file's neighbours already made once. */
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));

    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr" :: "a"((uint32_t)value),
                                "d"((uint32_t)(value >> 32)),
                                "c"(msr));
}

void user_init(void)
{
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);

    /*
     * Which selectors the two instructions load, and `gdt.h` explains why
     * the layout has no freedom in it: `syscall` takes CS from bits 47:32
     * and SS from that plus 8; `sysretq` takes SS from bits 63:48 plus 8
     * and CS from that plus 16.
     */
    wrmsr(MSR_STAR, ((uint64_t)SEL_SYSRET_BASE << 48)
                  | ((uint64_t)SEL_KERNEL_CODE << 32));

    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);

    /*
     * The flags cleared on entry, and IF is the one that has to be here.
     *
     * `syscall` does not switch the stack - that is the whole of what makes
     * its entry different from an exception's - so there are a few
     * instructions where the kernel is running on the *process's* stack. An
     * interrupt arriving there would push a frame onto memory a process
     * chose, at ring 0. AArch64 has no equivalent exposure: the hardware
     * selects SP_EL1 before the first instruction of the handler.
     *
     * DF because the ABI says a function is entered with it clear and a
     * process is free to set it, and AC so that alignment checking a
     * process turned on does not follow it into the kernel.
     */
    wrmsr(MSR_FMASK, (1UL << 9) | (1UL << 10) | (1UL << 18));
}
