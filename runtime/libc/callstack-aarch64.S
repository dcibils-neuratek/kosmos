/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * kosmos_call_on_stack(fn, arg, top) for AArch64: `fn(arg)` on the stack that
 * ends at `top`, and back on this one.
 *
 * For a vendored engine whose frames outgrow a process's stack - Quake's
 * renderer puts a 205 KB frame on it - without asking the kernel for a bigger
 * stack for every process. The caller owns that memory and its guard page;
 * this only moves `sp`.
 *
 * x19 carries the old `sp` across the call because it is callee-saved: `fn`
 * hands it back unchanged, which is the whole mechanism. It is pushed first,
 * beside the frame record, so this function hands x19 back too.
 *
 * `exit` from inside `fn` still lands where `kosmos_exit_arm` was armed on the
 * old stack: `longjmp` restores `sp` from its buffer, and the frames left on
 * the other stack are simply never returned to.
 */

.section .text
.global kosmos_call_on_stack

kosmos_call_on_stack:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    stp     x19, x20, [sp, #-16]!

    mov     x19, sp

    /* The new top, on the 16-byte boundary AAPCS64 requires of sp. */
    bic     x2, x2, #15
    mov     sp, x2

    mov     x16, x0
    mov     x0, x1
    blr     x16

    mov     sp, x19

    ldp     x19, x20, [sp], #16
    ldp     x29, x30, [sp], #16
    ret
