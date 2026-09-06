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

struct thread;

void fp_forget(struct thread *t)
{
    (void)t;
}
