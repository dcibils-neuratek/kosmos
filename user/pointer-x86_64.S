/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A driver that holds the pointer's left button down and ends, on x86-64.
 *
 * `pointer-aarch64.S` is the same process in the other instruction set. What
 * it is for belongs to the kernel rather than to either machine: a process
 * that reported a button with `SYS_POINTER_MOVE` and ended without letting go
 * - killed, faulted, or simply gone - must not leave the desktop dragging for
 * good. The USB driver is that process, and the board holds its buttons as
 * one source's (`hal/pc/pointer.c`).
 *
 * It exits with what the kernel answered, so the suite can tell a board that
 * took the button (0) from one whose pointer is absolute and refused it.
 */

#include "syscall.h"

.section .user.text, "ax"

.macro IMAGE_HEADER
    .quad   0x534f4d534f4b
    .quad   0x1000
.endm

.balign 4096
.global user_pointer_held_start
user_pointer_held_start:
    IMAGE_HEADER
    xorq    %rdi, %rdi              /* not moved across */
    xorq    %rsi, %rsi              /* nor down */
    movq    $1, %rdx                /* the left button, held */
    xorq    %r10, %r10              /* and no wheel */
    movq    $SYS_POINTER_MOVE, %rax
    syscall

    movq    %rax, %rdi              /* and ends, without letting go */
    movq    $SYS_EXIT, %rax
    syscall

    /* SYS_EXIT does not return. If it ever did, spinning here is better
     * than running off the end of the page into whatever follows. */
1:  jmp     1b

/* Padded to a whole page, so the header's "4096 bytes are read-only" is a
 * true statement about this blob rather than about it plus whatever the
 * linker put next. */
.balign 4096
.global user_pointer_held_end
user_pointer_held_end:
