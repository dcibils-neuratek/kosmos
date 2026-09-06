/*
 * setjmp and longjmp for x86-64.
 *
 * Saves exactly the System V callee-saved set, which is six registers and
 * two stack values against AArch64's twelve registers and eight FP halves -
 * and the difference is entirely the ABI rather than the machine.
 * **Every XMM register is caller-saved here**, so unlike `setjmp-aarch64.S`
 * there is no floating point in this buffer at all: a caller that had a
 * live double has already spilled it.
 *
 * The layout has to match jmp_buf in setjmp.h. There is no _Static_assert
 * reaching across into assembly, so the offsets are written out here rather
 * than hidden behind names that could drift.
 *
 *    0  rbx    8  rbp   16  r12   24  r13
 *   32  r14   40  r15   48  rsp   56  rip
 *
 * `rsp` is stored as it will be *after* the return - above the return
 * address - and `rip` is that address, so longjmp restores the stack and
 * jumps, exactly as `context_switch` does with the same two values and for
 * the same reason.
 */

.section .text
.global setjmp
.global longjmp

setjmp:
    movq    %rbx,  0(%rdi)
    movq    %rbp,  8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)

    /* The stack as the caller will see it once this returns, and where it
     * returns to. Both come off the top of the stack we are standing on. */
    leaq    8(%rsp), %rax
    movq    %rax, 48(%rdi)
    movq    (%rsp), %rax
    movq    %rax, 56(%rdi)

    xorl    %eax, %eax
    ret

longjmp:
    movq     0(%rdi), %rbx
    movq     8(%rdi), %rbp
    movq    16(%rdi), %r12
    movq    24(%rdi), %r13
    movq    32(%rdi), %r14
    movq    40(%rdi), %r15

    /*
     * The value first, because rax is about to be the only register left
     * that is not the caller's. A val of 0 becomes 1, as the standard
     * requires: setjmp has to be able to tell a direct call from a jump.
     */
    movl    %esi, %eax
    testl   %eax, %eax
    jnz     1f
    movl    $1, %eax
1:
    /*
     * The stack, then the jump. `jmp *` rather than pushing the address and
     * returning: pushing would write below the restored rsp, which is
     * memory the target frame does not expect to be touched.
     */
    movq    56(%rdi), %rdx
    movq    48(%rdi), %rsp
    jmpq    *%rdx
