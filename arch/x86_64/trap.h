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

/*
 * A fault that came from ring 3, after it has been reported.
 *
 * The distinction is the whole point of a microkernel and it is one line of
 * code, because the hardware already did the work: a process doing
 * something it may not is a dead process, not a dead machine.
 *
 * `arch/aarch64/trap.c` calls `process_exit` here directly, and this will
 * too once `kernel/process.c` builds on this architecture. Until then the
 * name is a seam and `arch/x86_64/main.c` fills it.
 */
void trap_user_fault(struct trapframe *f) __attribute__((noreturn));

#endif /* ARCH_X86_64_TRAP_H */
