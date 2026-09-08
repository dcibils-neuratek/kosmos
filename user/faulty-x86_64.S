/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Four processes that each do exactly one thing they are not allowed to,
 * on x86-64.
 *
 * `faulty-aarch64.S` is the same four in the other instruction set. What is
 * being proved is identical and belongs to the microkernel rather than to
 * either machine: a process that misbehaves dies, and nothing else does.
 *
 * The exit codes are distinctive on purpose. A blob that reaches its exit
 * has *not* faulted, and a test that only checked "the process is gone"
 * could not tell the two apart - so each one exits with a number nothing
 * else uses, and a test seeing that number knows the hardware let something
 * through.
 */

#include "syscall.h"

.section .user.text, "ax"

.macro IMAGE_HEADER
    .quad   0x534f4d534f4b
    .quad   0x1000
.endm

/*
 * The kernel's own address on this board.
 *
 * `boot/x86_64/kosmos.ld` puts the image at 1 MB, which is where a
 * multiboot loader can place it and where every PC kernel has lived since
 * the memory below it stopped being usable. The ARM blob reads 0x40080000
 * for the same reason and neither number means anything beyond "inside the
 * kernel's map, and mapped supervisor-only".
 */
#define KERNEL_ADDRESS  0x100000

/*
 * Reads address zero.
 *
 * Page 0 is deliberately left unmapped, so this is a page fault naming the
 * address rather than a read of whatever happens to be there.
 */
.balign 4096
.global user_fault_null_start
user_fault_null_start:
    IMAGE_HEADER
    xorq    %rax, %rax
    movq    (%rax), %rcx

    /* Unreachable. If it is ever reached the fault did not happen, and
     * exiting with a distinctive code says so rather than looking like a
     * clean run. */
    movq    $41, %rdi
    movq    $SYS_EXIT, %rax
    syscall
.balign 4096
.global user_fault_null_end
user_fault_null_end:

/*
 * Reads a kernel address.
 *
 * The page is there and is mapped; what stops this is the supervisor bit in
 * the page table entry, which is the hardware half of the separation the
 * whole system rests on.
 */
.balign 4096
.global user_fault_kernel_start
user_fault_kernel_start:
    IMAGE_HEADER
    movq    $KERNEL_ADDRESS, %rax
    movq    (%rax), %rcx

    movq    $42, %rdi
    movq    $SYS_EXIT, %rax
    syscall
.balign 4096
.global user_fault_kernel_end
user_fault_kernel_end:

/*
 * Writes to its own code.
 *
 * W^X for a process: the text is mapped executable and not writable, so
 * this is a protection fault rather than a translation one - the page is
 * there and this access is refused.
 */
.balign 4096
.global user_fault_text_start
user_fault_text_start:
    IMAGE_HEADER
    leaq    user_fault_text_start(%rip), %rax
    movq    $0, (%rax)

    movq    $43, %rdi
    movq    $SYS_EXIT, %rax
    syscall
.balign 4096
.global user_fault_text_end
user_fault_text_end:

/*
 * Hands the kernel a pointer into the kernel.
 *
 * Nothing about the hardware stops this: the kernel would dereference it at
 * ring 0, where the mapping is valid and privileged. Only the explicit
 * check in the syscall path does. This one is expected to *live*, and to
 * exit 0 only if it was refused.
 */
.balign 4096
.global user_bad_pointer_start
user_bad_pointer_start:
    IMAGE_HEADER
    movq    $KERNEL_ADDRESS, %rdi   /* a kernel address */
    movq    $16, %rsi               /* a plausible length */
    movq    $SYS_WRITE, %rax
    syscall

    /* SYS_ERR_FAULT is -101. Anything else means it was allowed. */
    cmpq    $-101, %rax
    jne     allowed

    xorq    %rdi, %rdi
    movq    $SYS_EXIT, %rax
    syscall

allowed:
    movq    $44, %rdi
    movq    $SYS_EXIT, %rax
    syscall
.balign 4096
.global user_bad_pointer_end
user_bad_pointer_end:
