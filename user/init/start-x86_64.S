/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Where a process begins, on x86-64.
 *
 * There is no loader, no dynamic linker and no argv. The kernel maps the
 * image, points the stack at the top of the region, and `iretq`s here with
 * every register zeroed but one.
 *
 * `start-aarch64.S` is the same thirty lines said in the other instruction
 * set, and the two differences are both about calling convention: the boot
 * word arrives in rdi rather than x0, and the stack has to be aligned by
 * hand because nothing called us.
 */

.section .text.start, "ax"
.global _start

_start:
    /* The boot word the kernel put in rdi, kept somewhere the zeroing loop
     * below will not touch. rbx is callee-saved, so `main` would preserve
     * it too - but nothing has been called yet and this is simply a
     * register that the loop does not want. */
    movq    %rdi, %rbx

    /*
     * Zero .bss. The image is a flat binary and the linker script emits
     * .bss as zeroes, so this is already true; doing it anyway costs a
     * handful of instructions once and means the image stops depending on
     * that particular property of how it was built.
     *
     * `rep stosq` rather than a loop, because on this architecture the loop
     * *is* an instruction. The count is in quadwords, and the linker
     * aligns both ends to 8 so the division is exact.
     */
    leaq    __bss_start(%rip), %rdi
    leaq    __bss_end(%rip), %rcx
    subq    %rdi, %rcx
    shrq    $3, %rcx
    xorq    %rax, %rax
    rep stosq

    /*
     * The stack, aligned before the call rather than after.
     *
     * System V wants rsp+8 to be a multiple of 16 at the *entry* of a
     * function, which is the state a `call` produces from a 16-aligned
     * rsp. Nothing called `_start`, so the kernel's stack top is whatever
     * it is and the `and` makes it 16 first. Getting this wrong is
     * invisible until something in `main` uses an aligned SSE store, and
     * then it is a general protection fault a long way from the cause.
     */
    andq    $-16, %rsp

    movq    %rbx, %rdi
    call    main

    /*
     * main returned. Exit with whatever it gave back rather than falling
     * into the bytes that happen to follow.
     *
     * rax is the syscall number and rdi already holds main's return value,
     * which is argument zero - so this is two instructions rather than
     * three.
     */
    movq    %rax, %rdi
    xorq    %rax, %rax              /* SYS_EXIT */
    syscall

1:  jmp     1b
