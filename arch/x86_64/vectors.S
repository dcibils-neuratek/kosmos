/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The exception stubs.
 *
 * AArch64 has sixteen entries in a table, four per exception class, and the
 * handler works out what happened by reading ESR_EL1. x86 has 256 separate
 * vectors and the processor tells you which one only by *which entry it
 * jumped through* - so there has to be a stub per vector whose whole job is
 * to write the number down.
 *
 * **Some vectors push an error code and some do not**, which is the detail
 * that makes this file look repetitive rather than be a loop. The stack the
 * handler sees has to have the same shape either way, so the ones that push
 * nothing push a zero in its place. Getting this wrong does not fault: it
 * misaligns every field after it, and the handler reports a plausible wrong
 * address for a fault that happened somewhere else.
 *
 * Which vectors carry a code is not a pattern to derive - it is a list in
 * the manual: 8, 10, 11, 12, 13, 14, 17, 21, 29 and 30.
 * Intel SDM volume 3, table 6-1.
 */

.section .text

.macro STUB_NOCODE vector
.global isr\vector
isr\vector:
    pushq   $0                      /* the error code this one does not have */
    pushq   $\vector
    jmp     isr_common
.endm

.macro STUB_CODE vector
.global isr\vector
isr\vector:
    pushq   $\vector                /* the processor pushed the code already */
    jmp     isr_common
.endm

    STUB_NOCODE 0                   /* divide error */
    STUB_NOCODE 1                   /* debug */
    STUB_NOCODE 2                   /* NMI */
    STUB_NOCODE 3                   /* breakpoint */
    STUB_NOCODE 4                   /* overflow */
    STUB_NOCODE 5                   /* bound range */
    STUB_NOCODE 6                   /* invalid opcode */
    STUB_NOCODE 7                   /* device not available */
    STUB_CODE   8                   /* double fault */
    STUB_NOCODE 9
    STUB_CODE   10                  /* invalid TSS */
    STUB_CODE   11                  /* segment not present */
    STUB_CODE   12                  /* stack-segment fault */
    STUB_CODE   13                  /* general protection */
    STUB_CODE   14                  /* page fault */
    STUB_NOCODE 15
    STUB_NOCODE 16                  /* x87 floating point */
    STUB_CODE   17                  /* alignment check */
    STUB_NOCODE 18                  /* machine check */
    STUB_NOCODE 19                  /* SIMD floating point */
    STUB_NOCODE 20                  /* virtualisation */
    STUB_CODE   21                  /* control protection */
    STUB_NOCODE 22
    STUB_NOCODE 23
    STUB_NOCODE 24
    STUB_NOCODE 25
    STUB_NOCODE 26
    STUB_NOCODE 27
    STUB_NOCODE 28
    STUB_CODE   29                  /* VMM communication */
    STUB_CODE   30                  /* security */
    STUB_NOCODE 31

/*
 * And the hardware interrupts, 32 upward, which the PIC was remapped to.
 *
 * No error code is ever pushed for these, so they all take the NOCODE form
 * and the frame the handler sees is the same shape as an exception's. That
 * sameness is the point: one `isr_common`, one `struct trapframe`, and
 * `trap_handle` decides what it is looking at from the vector number.
 */
    STUB_NOCODE 32                  /* the timer */
    STUB_NOCODE 33                  /* keyboard */
    STUB_NOCODE 34
    STUB_NOCODE 35
    STUB_NOCODE 36
    STUB_NOCODE 37
    STUB_NOCODE 38
    STUB_NOCODE 39
    STUB_NOCODE 40
    STUB_NOCODE 41
    STUB_NOCODE 42
    STUB_NOCODE 43
    STUB_NOCODE 44
    STUB_NOCODE 45
    STUB_NOCODE 46
    STUB_NOCODE 47

/*
 * Everything the handler is allowed to look at, and everything it must put
 * back.
 *
 * The order here is the order `struct trapframe` declares, backwards -
 * `pushq` grows down, so the last thing pushed is the first field. Getting
 * the two out of step is a silent bug: the C sees `rax` where `r15` is.
 */
isr_common:
    pushq   %r15
    pushq   %r14
    pushq   %r13
    pushq   %r12
    pushq   %r11
    pushq   %r10
    pushq   %r9
    pushq   %r8
    pushq   %rbp
    pushq   %rdi
    pushq   %rsi
    pushq   %rdx
    pushq   %rcx
    pushq   %rbx
    pushq   %rax

    /* The frame itself as the argument. The System V ABI puts the first
     * one in rdi, and rsp is pointing at exactly what C wants to see. */
    movq    %rsp, %rdi
    call    trap_handle

    popq    %rax
    popq    %rbx
    popq    %rcx
    popq    %rdx
    popq    %rsi
    popq    %rdi
    popq    %rbp
    popq    %r8
    popq    %r9
    popq    %r10
    popq    %r11
    popq    %r12
    popq    %r13
    popq    %r14
    popq    %r15

    /* Past the vector and the error code, which the processor will not do
     * for us: `iretq` expects to find RIP where they are sitting. */
    addq    $16, %rsp
    iretq
