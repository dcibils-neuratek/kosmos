/*
 * Programs that misbehave on purpose.
 *
 * Each one is a separate blob so a process can be built from exactly one of
 * them, and each is position independent for the same reason `hello.S` is:
 * it is copied into a page and mapped somewhere unrelated to where it was
 * linked.
 *
 * The first three must not survive. The fourth must, because being told no
 * is not the same as being killed: a syscall that refuses a bad pointer has
 * to return an error, not take the process down and certainly not the
 * kernel.
 */

#include "syscall.h"

.section .user.text, "ax"

/* Dereferences address zero. The classic, and M4's definition of done. */
.global user_fault_null_start
user_fault_null_start:
    mov     x0, xzr
    ldr     x1, [x0]

    /* Unreachable. If it is ever reached the fault did not happen, and
     * exiting with a distinctive code says so rather than looking like a
     * clean run. */
    mov     x0, #41
    mov     x8, #SYS_EXIT
    svc     #0
.global user_fault_null_end
user_fault_null_end:

/*
 * Reads a kernel address.
 *
 * 0x40080000 is where the image is loaded, so it is mapped and valid - at
 * EL1. From EL0 the AP bits make it inaccessible, and that is the whole of
 * the isolation: the page is right there in this process's own tables and
 * still cannot be touched.
 */
.global user_fault_kernel_start
user_fault_kernel_start:
    movz    x0, #0x4008, lsl #16
    ldr     x1, [x0]

    mov     x0, #42
    mov     x8, #SYS_EXIT
    svc     #0
.global user_fault_kernel_end
user_fault_kernel_end:

/*
 * Writes to its own code.
 *
 * Mapped EL0 read-only and executable. A process that could rewrite its own
 * text could defeat anything the kernel checked about it beforehand.
 */
.global user_fault_text_start
user_fault_text_start:
    adr     x0, user_fault_text_start
    mov     x1, #0
    str     x1, [x0]

    mov     x0, #43
    mov     x8, #SYS_EXIT
    svc     #0
.global user_fault_text_end
user_fault_text_end:

/*
 * Hands a kernel pointer to a syscall.
 *
 * Nothing about the hardware stops this: the kernel would dereference it at
 * EL1, where the mapping is valid and privileged. Only the explicit check in
 * the syscall path does. This one is expected to live, and to exit 0 only if
 * it was refused.
 */
.global user_bad_pointer_start
user_bad_pointer_start:
    movz    x0, #0x4008, lsl #16    /* a kernel address */
    mov     x1, #16                 /* a plausible length */
    mov     x8, #SYS_WRITE
    svc     #0

    /* SYS_ERR_FAULT is -101. Anything else means it was allowed. */
    mov     x1, #-101
    cmp     x0, x1
    b.ne    allowed

    mov     x0, #0
    mov     x8, #SYS_EXIT
    svc     #0

allowed:
    mov     x0, #44
    mov     x8, #SYS_EXIT
    svc     #0
.global user_bad_pointer_end
user_bad_pointer_end:
