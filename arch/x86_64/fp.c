/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Who owns the floating-point registers, and on this machine the answer is
 * still "whoever last touched them".
 *
 * `arch/aarch64/fp.c` moves them lazily: the switch disarms FP, the first
 * instruction that wants them traps, and the handler carries the register
 * file from its previous owner to its new one. That costs `context_switch`
 * 36% to do eagerly and nothing at all to do this way for a thread that
 * never uses FP - which is every kernel thread, because the kernel's own C
 * cannot emit an FP instruction.
 *
 * **The same mechanism exists here and is not built yet.** `CR0.TS` is the
 * disarm, the fault is vector 7 - which `trap.c` already calls "device not
 * available", the name it was given in 1985 for exactly this - and
 * `FXSAVE` is what moves the file. `context.h` says why the 512-byte area
 * is not in `struct context` in the meantime: a field nothing reads is
 * indistinguishable from a bug, and the pieces only make sense together.
 *
 * So `fp_forget` has nothing to forget, and that is the correct answer
 * rather than a placeholder. It exists because `kernel/thread.c` calls it
 * when a thread dies, so that a later fault cannot save a dead thread's
 * registers into a slot that now belongs to somebody else. There is no
 * `owner` here to clear because nothing is tracked, and there is nothing to
 * track because the switch preserves nothing lazily.
 *
 * What makes that safe today rather than lucky: System V on AMD64 makes
 * every XMM register caller-saved, so a thread that *called* the switch has
 * already had the compiler spill whatever it cared about. What is not safe
 * is preemption, and nothing here preempts yet. When it does, this file
 * grows the handler and this function grows a line.
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
