/*
 * The first thing Kosmos runs at ring 3, on x86-64.
 *
 * `hello-aarch64.S` is the same program in the other instruction set, and
 * the differences are all convention rather than idea: the syscall number
 * goes in rax rather than x8, the arguments in rdi and rsi rather than x0
 * and x1, and the instruction is `syscall` rather than `svc`.
 *
 * Assembly rather than C, and position independent, because it is copied
 * into a fresh page and mapped at an address that has nothing to do with
 * where it was linked. `leaq ...(%rip)` is the PC-relative form, so the
 * string is found wherever the blob ends up; an absolute reference would
 * point back into the kernel image, which this process cannot reach and
 * should not be able to.
 *
 * It has no libc, no stack frame, and no way home. It writes, and it exits.
 */

#include "syscall.h"

.section .user.text, "ax"

/*
 * The image header every process image carries: a magic number, then how
 * many bytes are read-only and executable. These blobs are entirely code
 * and fit in a page, so the answer is always one page.
 *
 * Real images get theirs from the linker script, which knows where rodata
 * ends. A hand-written blob has to say it by hand, and saying it is better
 * than the kernel having a second path that skips the check.
 */
.macro IMAGE_HEADER
    .quad   0x534f4d534f4b
    .quad   0x1000
.endm

/*
 * **A page, because the kernel maps the read-only half where it lies.**
 *
 * There is one physical copy of a process image's code, mapped into every
 * address space that runs it, and a mapping starts at a page boundary or it
 * starts at the wrong bytes. `process_create` refuses an image that is not
 * page-aligned.
 */
.balign 4096
.global user_hello_start
user_hello_start:
    IMAGE_HEADER
    leaq    message(%rip), %rdi
    movq    $(message_end - message), %rsi
    movq    $SYS_WRITE, %rax
    syscall

    /* And again, with the result of the first call, to prove the return
     * value comes back across the boundary rather than being ignored. */
    cmpq    $(message_end - message), %rax
    jne     wrong

    xorq    %rdi, %rdi
    movq    $SYS_EXIT, %rax
    syscall

wrong:
    movq    $1, %rdi
    movq    $SYS_EXIT, %rax
    syscall

    /* SYS_EXIT does not return. If it ever did, spinning here is better
     * than running off the end of the page into whatever follows. */
1:  jmp     1b

message:
    .ascii  "hello from ring 3\n"
message_end:

/* Padded to a whole page, so the header's "4096 bytes are read-only" is a
 * true statement about this blob rather than about it plus whatever the
 * linker put next. */
.balign 4096
.global user_hello_end
user_hello_end:
