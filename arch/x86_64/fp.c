/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Turning the floating-point unit on, and nothing else - because on this
 * board the context switch already saves it.
 *
 * `arch/aarch64/fp.c` moves the registers *lazily*: the switch disarms FP,
 * the first instruction that wants them traps, and the handler carries the
 * file from its previous owner to its new one. That is worth 36% of a
 * context switch and 17% of an IPC round trip for a thread that never
 * touches an FP register, which is every kernel thread, because the
 * kernel's own C cannot emit one.
 *
 * **Here the switch is eager, on purpose, and that is what makes preemption
 * safe.** `switch.S` does `fxsave` and `fxrstor` around every switch and
 * `context.h` carries the 512-byte area they need, with the argument for
 * why: the lazy mechanism would be CR0.TS, vector 7 - which `trap.c`
 * already calls "device not available", the name it was given in 1985 for
 * exactly this - and FXSAVE, and every piece of that is understood. What
 * does not exist is the *measurement*, and this system does not push work
 * down without one. 36% of a number nobody has taken is not an argument.
 *
 * **This comment used to say the opposite, and it was wrong three times
 * over**: that the mechanism was not built, that the 512-byte area was not
 * in `struct context`, and - the dangerous one - that "preemption is not
 * safe, and nothing here preempts yet". Both halves of that were false by
 * then: the timer preempts on this board like any other, and the eager
 * save is precisely what makes it correct. It was written before `switch.S`
 * grew those two instructions and nothing went back to it, which is the
 * failure this project keeps finding in prose and never in code.
 *
 * So `fp_forget` has nothing to forget, and that is the correct answer
 * rather than a placeholder: there is no lazy owner to clear because
 * nothing is deferred. It exists because `kernel/thread.c` calls it when a
 * thread dies, and it will grow a line on the day the file moves lazily.
 */

#include <stdint.h>

#include "context.h"
#include "cpu.h"

/*
 * **Whether `switch.S` uses XSAVE**, and with it AVX's registers (8
 * November 2026, `docs/xavier.md`, *Making it fast*). Set by `fp_init` on
 * every core, the same on each since they are the same processor, before
 * any thread is switched; read by the switch on every switch after.
 *
 * What XCR0 names is what a thread may use: x87 and SSE always, and AVX
 * where CPUID says the processor has it - which is what lets a program
 * ask CPUID and XGETBV whether AVX2 is there, as the LLM Kit does, and get
 * an answer the switch keeps true. Nothing past AVX: AVX-512 is another
 * 1.6 KB a thread, and no processor Kosmos runs on has it.
 */
unsigned char fp_xsave;

#define CPUID1_ECX_XSAVE (1u << 26)
#define CPUID1_ECX_AVX   (1u << 28)
#define XCR0_X87         (1u << 0)
#define XCR0_SSE         (1u << 1)
#define XCR0_AVX         (1u << 2)

/*
 * Turning the floating-point unit on, which x86 requires and ARM does not.
 *
 * An AArch64 core comes up able to execute FP instructions and `mmu_init`
 * never mentions them. Here, SSE raises #UD until CR4.OSFXSR says the
 * operating system knows how to save the state - the bit is literally an
 * assertion by this file that `context_switch` does what it does - and
 * CR0.EM has to be clear or every FP instruction is emulated into a trap
 * that nothing handles.
 *
 * CR0.MP with EM clear means WAIT faults only when the task-switched flag
 * is set, which is the pair the lazy scheme above would use. Setting it now
 * costs nothing and is what the manual asks for.
 */
void fp_init(void)
{
    uint64_t cr0, cr4;

    __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1UL << 2);                 /* EM: no emulation */
    cr0 |=  (1UL << 1);                 /* MP: monitor coprocessor */
    __asm__ volatile("movq %0, %%cr0" :: "r"(cr0));

    __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
    cr4 |= (1UL << 9) | (1UL << 10);    /* OSFXSR, OSXMMEXCPT */
    __asm__ volatile("movq %0, %%cr4" :: "r"(cr4));

    /*
     * XSAVE, where there is one. CR4.OSXSAVE is this file saying the switch
     * keeps what XCR0 names, which is what makes XSETBV legal and AVX
     * executable; and the size CPUID gives for that XCR0 is held to the
     * room `context.h` has, so a processor that wanted more is refused
     * XSAVE rather than given an area that overruns into the next thread.
     */
    {
        uint32_t r[4], xcr0 = XCR0_X87 | XCR0_SSE;

        cpu_cpuid(1, 0, r);

        if (r[2] & CPUID1_ECX_XSAVE) {
            if (r[2] & CPUID1_ECX_AVX) {
                xcr0 |= XCR0_AVX;
            }

            __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
            cr4 |= (1UL << 18);                     /* OSXSAVE */
            __asm__ volatile("movq %0, %%cr4" :: "r"(cr4));
            __asm__ volatile("xsetbv" :: "c"(0), "a"(xcr0), "d"(0));

            cpu_cpuid(0xd, 0, r);                   /* EBX: the size for XCR0 now */

            if (r[1] != 0 && r[1] <= CTX_FX_SIZE) {
                fp_xsave = 1;
            } else {
                __asm__ volatile("xsetbv" :: "c"(0), "a"(XCR0_X87 | XCR0_SSE), "d"(0));
            }
        }
    }

    /* And a known x87 state, so the first save writes something sane
     * rather than whatever the firmware left. */
    __asm__ volatile("fninit");
}

struct thread;

void fp_forget(struct thread *t)
{
    (void)t;
}

/* Nothing to give back either: a switch here saves the registers it leaves
 * (`fp_hand_back` on AArch64 says what the lazy one has to do). */
void fp_hand_back(struct thread *t)
{
    (void)t;
}
