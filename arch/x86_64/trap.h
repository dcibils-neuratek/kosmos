/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef ARCH_X86_64_TRAP_H
#define ARCH_X86_64_TRAP_H

#include <stdbool.h>
#include <setjmp.h>
#include <stdint.h>

/*
 * What an exception left on the stack.
 *
 * The bottom fifteen are pushed by `isr_common`, in reverse; the top five
 * are pushed by the processor itself and their order is architectural. The
 * declaration order here *is* the memory order, which is why this struct
 * and `vectors.S` have to be read together and why neither can be tidied
 * alone.
 */
struct trapframe {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;

    uint64_t vector;
    uint64_t error;

    /* The processor's own, and it always pushes all five in long mode -
     * including SS and RSP, which 32-bit x86 pushed only on a privilege
     * change. That difference is a classic source of a handler that works
     * from the kernel and corrupts the stack when it comes from a process. */
    uint64_t rip, cs, rflags, rsp, ss;
};

void trap_init(void);
void trap_handle(struct trapframe *f);

/*
 * Deliberate faults, for tests.
 *
 * The same need as `arch/aarch64/trap.h`'s, and deliberately not the same
 * shape - because **on x86-64 you cannot step past a faulting
 * instruction.** ARM's handler does `elr += 4` and the arithmetic is exact,
 * every A64 instruction being four bytes. Here an instruction is one to
 * fifteen bytes and its length cannot be known without decoding it, so
 * there is no next instruction to step to and no honest way to invent one.
 *
 * So this architecture gets only the *unwind* form, which ARM already has
 * for "faults you cannot simply step over" - the stack-overflow case, where
 * resuming would only fault again. Here that case is every case:
 *
 *     jmp_buf env;
 *
 *     if (setjmp(env) == 0) {
 *         fault_expect_unwind(env);
 *         ... the thing that should fault ...
 *     }
 *
 *     if (!fault_expect_end(&info)) { ... it did not fault ... }
 *
 * `tests/fault.h` wraps that in one macro so a shared test can say what it
 * means without saying which board it is on.
 *
 * Not reentrant, and not meant to be: one armed fault at a time. Nesting
 * would mean a fault inside the handler, which is a double fault and should
 * be a panic rather than a feature.
 */
struct fault_info {
    uint64_t vector;        /* which exception: 6 is #UD, 14 is #PF */
    uint64_t error;         /* the code the processor pushed, or 0 */
    uint64_t rip;           /* the instruction that faulted */
    uint64_t cr2;           /* the address it touched; only for #PF */
    uint64_t handler_sp;    /* which stack the handler itself ran on */
};

/*
 * Arm the slot. Control resumes at the matching `setjmp` with a return
 * value of 1.
 *
 * The handler does not call `longjmp` itself, and cannot: it is in the
 * middle of an exception and returning from C would run off the end of
 * `isr_common` without the `iret`. What it does instead is rewrite the
 * trapframe so the `iret` *itself* lands in `longjmp` with the right
 * arguments - rdi and rsi are restored by the same pops that restore
 * everything else, so setting all three is a complete call.
 *
 * `rsp` is left where the fault found it, which may be inside a guard page.
 * That is safe for the reason it is safe on ARM: `longjmp` touches no stack
 * at all before it sets one, only loading from the buffer.
 */
void fault_expect_unwind(jmp_buf env);

/* Disarms, and returns whether a fault actually fired. Fills *out when it
 * did; *out is untouched otherwise. */
bool fault_expect_end(struct fault_info *out);


/* One line for the boot log, because `kernel/main.c` printed a string
 * literal about this architecture's hardware until there were two. */
const char *trap_describe(void);

#endif /* ARCH_X86_64_TRAP_H */
