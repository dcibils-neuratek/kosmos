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
    /*
     * **.bss is not zeroed here, and it was.** The image carries it as
     * zeroes (`user.ld` emits it rather than leaving it NOLOAD) and the
     * kernel gives each page of the writable half as a copy of the image's
     * page there the first time it is touched (`process_fill`), so it is
     * already zero. A loop writing it again touched every page of it in
     * every process - 3.4 MB each, most of it FFmpeg's tables in processes
     * that never decode anything - so as not to depend on how the image was
     * built. That is the kernel's guarantee now, and the kernel suite's
     * `el0: memory is given on first touch` holds it. The boot word is
     * still where the kernel put it, since nothing here uses a register.
     */

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
