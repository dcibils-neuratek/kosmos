/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A driver that holds the pointer's left button down and ends, on AArch64.
 *
 * `pointer-x86_64.S` is the same process in the other instruction set, and
 * says what it is for. On this board the pointer is a virtio tablet, which
 * says where it is and has no room for a button reported from a process, so
 * the kernel refuses the report - and the process exits with that refusal,
 * which is what the suite expects here.
 */

#include "syscall.h"

.section .user.text, "ax"

.macro IMAGE_HEADER
    /* The alignment goes before the label rather than here, for the reason
     * `hello-aarch64.S` gives. */
    .quad   0x534f4d534f4b
    .quad   0x1000
.endm

.balign 4096
.global user_pointer_held_start
user_pointer_held_start:
    IMAGE_HEADER
    mov     x0, #0                  /* not moved across */
    mov     x1, #0                  /* nor down */
    mov     x2, #1                  /* the left button, held */
    mov     x3, #0                  /* and no wheel */
    mov     x8, #SYS_POINTER_MOVE
    svc     #0

    /* x0 is what the kernel answered, and it ends with that, without
     * letting go. */
    mov     x8, #SYS_EXIT
    svc     #0

    /* SYS_EXIT does not return. If it ever did, spinning here is better than
     * running off the end of the page into whatever follows. */
1:  b       1b

/* Padded to a whole page, so the header's "4096 bytes are read-only" is a
 * true statement about this blob rather than about it plus whatever the
 * linker put next. */
.balign 4096
.global user_pointer_held_end
user_pointer_held_end:
