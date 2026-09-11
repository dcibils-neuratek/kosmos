/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * kosmos_call_on_stack(fn, arg, top) for x86-64: the move
 * `callstack-aarch64.S` makes, with rbx carrying the old `rsp` because it is
 * callee-saved - `fn` hands it back unchanged.
 *
 * System V wants `rsp` on a 16-byte boundary at the `call`, so that the
 * callee starts eight below one with its return address pushed. Rounding
 * `top` down and calling is exactly that.
 */

.section .text
.global kosmos_call_on_stack

kosmos_call_on_stack:
    pushq   %rbp
    movq    %rsp, %rbp
    pushq   %rbx

    movq    %rsp, %rbx

    andq    $-16, %rdx
    movq    %rdx, %rsp

    movq    %rdi, %rax
    movq    %rsi, %rdi
    callq   *%rax

    movq    %rbx, %rsp

    popq    %rbx
    popq    %rbp
    ret
