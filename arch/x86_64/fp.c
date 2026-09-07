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

    /* And a known x87 state, so the first FXSAVE writes something sane
     * rather than whatever the firmware left. */
    __asm__ volatile("fninit");
}

struct thread;

void fp_forget(struct thread *t)
{
    (void)t;
}
