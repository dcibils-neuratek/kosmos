/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef SETJMP_H
#define SETJMP_H

/*
 * The one piece of the libc that has to be exactly right the first time.
 *
 * Lua uses setjmp/longjmp for every error it raises, so a mistake here is
 * invisible until something calls error() for the first time, which can be
 * weeks after this was written. `setup.md` warns about it and it is worth
 * the warning.
 *
 * The buffer holds whatever the ABI makes the callee's responsibility, and
 * the two architectures disagree about how much that is.
 *
 * AAPCS64: x19 to x28, x29 and x30, sp, and d8 to d15 - twenty-two slots.
 * The FP halves are in there even though the kernel is built with
 * -mgeneral-regs-only and cannot touch them, because Lua's numbers are
 * doubles and a longjmp crossing a frame with a live d8 would corrupt it.
 * The flag restricts what the compiler emits, not what hand-written
 * assembly may save, so the correct thing costs nothing.
 *
 * System V on AMD64: rbx, rbp, r12 to r15, rsp and rip - eight slots, and
 * **no floating point at all**, because every XMM register is caller-saved
 * there. A caller that had a live double has already spilled it. That is
 * the same reason `arch/x86_64/switch.S` saves no FP where the ARM one has
 * to think about it.
 *
 * Sized to the larger, because a jmp_buf is a type in a header that both
 * compile against and the cost of the difference is 112 bytes on a
 * structure there are a handful of.
 */
typedef unsigned long jmp_buf[22];

/* Returns 0 when called directly, and the value passed to longjmp when
 * arriving from one. */
int setjmp(jmp_buf env);

/* Never returns. A val of 0 is turned into 1, as the standard requires:
 * setjmp has to be able to tell a direct call from a jump. */
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#endif /* SETJMP_H */
