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

/*
 * The image header every process image carries: a magic number, then how
 * many bytes are read-only and executable. These blobs are entirely code and
 * fit in a page, so the answer is always one page.
 *
 * Real images get theirs from the linker script, which knows where rodata
 * ends. A hand-written blob has to say it by hand, and saying it is better
 * than the kernel having a second path that skips the check.
 */
.macro IMAGE_HEADER
    .quad   0x534f4d534f4b
    .quad   0x1000
.endm


/* Dereferences address zero. The classic, and M4's definition of done. */
/*
 * **A page, because the kernel maps the read-only half where it lies.**
 *
 * There is one physical copy of a process image's code, mapped into every
 * address space that runs it, and a mapping starts at a page boundary or it
 * starts at the wrong bytes. `.balign 16` was enough while the image was
 * copied into fresh pages; `process_create` now refuses one that is not
 * page-aligned, which is how this said so rather than running on whatever
 * preceded it.
 */
.balign 4096
.global user_fault_null_start
user_fault_null_start:
    IMAGE_HEADER
    mov     x0, xzr
    ldr     x1, [x0]

    /* Unreachable. If it is ever reached the fault did not happen, and
     * exiting with a distinctive code says so rather than looking like a
     * clean run. */
    mov     x0, #41
    mov     x8, #SYS_EXIT
    svc     #0
/* Padded to a whole page, so the header's "4096 bytes are read-only" is a
 * true statement about this blob rather than about it plus whatever the
 * linker put next. The kernel maps the read-only half where it lies, so
 * anything inside that page is readable by the process - and `process_create`
 * refuses an image shorter than the half it declares, which is what would
 * catch this if it were ever undone. */
.balign 4096
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
/*
 * **A page, because the kernel maps the read-only half where it lies.**
 *
 * There is one physical copy of a process image's code, mapped into every
 * address space that runs it, and a mapping starts at a page boundary or it
 * starts at the wrong bytes. `.balign 16` was enough while the image was
 * copied into fresh pages; `process_create` now refuses one that is not
 * page-aligned, which is how this said so rather than running on whatever
 * preceded it.
 */
.balign 4096
.global user_fault_kernel_start
user_fault_kernel_start:
    IMAGE_HEADER
    movz    x0, #0x4008, lsl #16
    ldr     x1, [x0]

    mov     x0, #42
    mov     x8, #SYS_EXIT
    svc     #0
/* Padded to a whole page, so the header's "4096 bytes are read-only" is a
 * true statement about this blob rather than about it plus whatever the
 * linker put next. The kernel maps the read-only half where it lies, so
 * anything inside that page is readable by the process - and `process_create`
 * refuses an image shorter than the half it declares, which is what would
 * catch this if it were ever undone. */
.balign 4096
.global user_fault_kernel_end
user_fault_kernel_end:

/*
 * Writes to its own code.
 *
 * Mapped EL0 read-only and executable. A process that could rewrite its own
 * text could defeat anything the kernel checked about it beforehand.
 */
/*
 * **A page, because the kernel maps the read-only half where it lies.**
 *
 * There is one physical copy of a process image's code, mapped into every
 * address space that runs it, and a mapping starts at a page boundary or it
 * starts at the wrong bytes. `.balign 16` was enough while the image was
 * copied into fresh pages; `process_create` now refuses one that is not
 * page-aligned, which is how this said so rather than running on whatever
 * preceded it.
 */
.balign 4096
.global user_fault_text_start
user_fault_text_start:
    IMAGE_HEADER
    adr     x0, user_fault_text_start
    mov     x1, #0
    str     x1, [x0]

    mov     x0, #43
    mov     x8, #SYS_EXIT
    svc     #0
/* Padded to a whole page, so the header's "4096 bytes are read-only" is a
 * true statement about this blob rather than about it plus whatever the
 * linker put next. The kernel maps the read-only half where it lies, so
 * anything inside that page is readable by the process - and `process_create`
 * refuses an image shorter than the half it declares, which is what would
 * catch this if it were ever undone. */
.balign 4096
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
/*
 * **A page, because the kernel maps the read-only half where it lies.**
 *
 * There is one physical copy of a process image's code, mapped into every
 * address space that runs it, and a mapping starts at a page boundary or it
 * starts at the wrong bytes. `.balign 16` was enough while the image was
 * copied into fresh pages; `process_create` now refuses one that is not
 * page-aligned, which is how this said so rather than running on whatever
 * preceded it.
 */
.balign 4096
.global user_bad_pointer_start
user_bad_pointer_start:
    IMAGE_HEADER
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
/* Padded to a whole page, so the header's "4096 bytes are read-only" is a
 * true statement about this blob rather than about it plus whatever the
 * linker put next. The kernel maps the read-only half where it lies, so
 * anything inside that page is readable by the process - and `process_create`
 * refuses an image shorter than the half it declares, which is what would
 * catch this if it were ever undone. */
.balign 4096
.global user_bad_pointer_end
user_bad_pointer_end:
