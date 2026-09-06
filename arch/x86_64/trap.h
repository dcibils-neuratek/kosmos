/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef ARCH_X86_64_TRAP_H
#define ARCH_X86_64_TRAP_H

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


/* One line for the boot log, because `kernel/main.c` printed a string
 * literal about this architecture's hardware until there were two. */
const char *trap_describe(void);

#endif /* ARCH_X86_64_TRAP_H */
