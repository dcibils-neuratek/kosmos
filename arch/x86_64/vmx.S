/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Into a guest and back, VT-x - the x86-64 half of `docs/sandboxes.md` X1,
 * and the counterpart of `arch/aarch64/hyp.S`.
 *
 *   long vmx_run(uint64_t *gpr);    0: the guest exited; 1: it never ran
 *
 * Called by `arch_vcpu_run` with interrupts off and the vCPU's VMCS
 * current, its host and guest state written. What the VMCS does not hold
 * is the general registers but RSP, so they are loaded from `gpr` - in the
 * instruction set's order, rax rcx rdx rbx rsp rbp rsi rdi r8-r15, as
 * `struct vcpu_regs` keeps them - and stored back into it after the exit.
 * RSP, RIP and RFLAGS are the VMCS's, and C reads them.
 *
 * **The exit arrives at label 1 with nothing but a stack.** The processor
 * loads HOST_RSP and HOST_RIP and every general register is the guest's,
 * so the stack is where the way back is kept: the callee-saved set as
 * System V asks, and `gpr` on top, where the exit finds it. HOST_RSP is
 * written here, after those pushes, because only here is it known - and
 * nothing is pushed between that and `vmlaunch`.
 *
 * Every entry is a `vmlaunch`, never a `vmresume`: `arch_vcpu_run` clears
 * the VMCS after each run, so a vCPU can be run next by a thread on
 * another core (`vcpu.c` says why that is the simpler of the two).
 *
 * Intel SDM vol. 3, §26 (VM entries) and §27 (VM exits).
 */

#define HOST_RSP    0x6c14
#define HOST_RIP    0x6c16

    .text
    .globl  vmx_run
    .type   vmx_run, @function
vmx_run:
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    pushq   %rdi                    /* gpr, for the way back */

    movq    $HOST_RSP, %rax
    vmwrite %rsp, %rax
    jna     2f
    leaq    1f(%rip), %rdx
    movq    $HOST_RIP, %rax
    vmwrite %rdx, %rax
    jna     2f

    movq    0(%rdi), %rax
    movq    8(%rdi), %rcx
    movq    16(%rdi), %rdx
    movq    24(%rdi), %rbx
    movq    40(%rdi), %rbp
    movq    48(%rdi), %rsi
    movq    64(%rdi), %r8
    movq    72(%rdi), %r9
    movq    80(%rdi), %r10
    movq    88(%rdi), %r11
    movq    96(%rdi), %r12
    movq    104(%rdi), %r13
    movq    112(%rdi), %r14
    movq    120(%rdi), %r15
    movq    56(%rdi), %rdi          /* last: it was the pointer */

    vmlaunch
    jmp     2f                      /* only if the entry failed */

1:  /* the exit: RSP is HOST_RSP, and (%rsp) is gpr */
    pushq   %rdi
    movq    8(%rsp), %rdi
    movq    %rax, 0(%rdi)
    movq    %rcx, 8(%rdi)
    movq    %rdx, 16(%rdi)
    movq    %rbx, 24(%rdi)
    movq    %rbp, 40(%rdi)
    movq    %rsi, 48(%rdi)
    movq    %r8, 64(%rdi)
    movq    %r9, 72(%rdi)
    movq    %r10, 80(%rdi)
    movq    %r11, 88(%rdi)
    movq    %r12, 96(%rdi)
    movq    %r13, 104(%rdi)
    movq    %r14, 112(%rdi)
    movq    %r15, 120(%rdi)
    popq    %rax
    movq    %rax, 56(%rdi)          /* the guest's rdi */
    addq    $8, %rsp
    xorl    %eax, %eax
    jmp     3f

2:  addq    $8, %rsp                /* gpr */
    movl    $1, %eax

3:  popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret
    .size   vmx_run, . - vmx_run

