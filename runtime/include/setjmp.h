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
 * The buffer holds what AAPCS64 makes the callee's responsibility:
 *
 *   x19 to x28    the callee-saved general registers
 *   x29, x30      frame pointer and link register
 *   sp
 *   d8 to d15     the callee-saved halves of the FP registers
 *
 * The FP registers are in there even though the kernel is built with
 * -mgeneral-regs-only and cannot touch them. Lua's numbers are doubles, so
 * the moment Lua is compiled in, a longjmp that crossed a frame holding a
 * live d8 would corrupt it. The flag restricts what the compiler emits, not
 * what hand-written assembly may save, so the correct thing costs nothing.
 *
 *   22 slots x 8 bytes = 176 bytes.
 */
typedef unsigned long jmp_buf[22];

/* Returns 0 when called directly, and the value passed to longjmp when
 * arriving from one. */
int setjmp(jmp_buf env);

/* Never returns. A val of 0 is turned into 1, as the standard requires:
 * setjmp has to be able to tell a direct call from a jump. */
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#endif /* SETJMP_H */
