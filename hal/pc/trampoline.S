/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Where a started processor begins: real mode, on a page under 1 MB.
 *
 * **How a PC starts a processor, which is the board's half.** A STARTUP
 * inter-processor interrupt begins execution at vector * 0x1000 in real mode
 * with nothing else set up, so this page does one thing: a GDT of its own,
 * protected mode with flat segments, and a jump to the entry the kernel wrote
 * below with the context it wrote beside it in edi. Getting from there to
 * long mode is the architecture's - `boot/x86_64/start.S` does it for core
 * zero and does it again for every core that lands here.
 *
 * In real mode every address is an offset from CS's page, so the one
 * instruction that names memory there uses `x - trampoline_start`; after
 * that the segments are flat, and `AT(x)` is where `x` landed once copied.
 *
 * Intel SDM volume 3, section 9.4 for the MP start-up protocol.
 */

#include "trampoline.h"

#define AT(x)   (TRAMPOLINE_BASE + ((x) - trampoline_start))

    .section .rodata.trampoline, "a"

    .code16
    .global trampoline_start
trampoline_start:
    cli
    cld

    movw    %cs, %ax
    movw    %ax, %ds

    /* The first thing a started processor says: it ran at all. DS is this
     * page, so the offset is the address. */
    movl    $REACHED_REAL_MODE, TRAMPOLINE_REACHED

    lgdtl   gdt_pointer - trampoline_start

    movl    %cr0, %eax
    orl     $1, %eax                        /* PE */
    movl    %eax, %cr0

    ljmpl   $0x08, $AT(protected)

    .code32
protected:
    movl    $0x10, %eax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss

    movl    $REACHED_PROTECTED, TRAMPOLINE_BASE + TRAMPOLINE_REACHED

    /* And where the architecture should write how much further it gets. */
    movl    $(TRAMPOLINE_BASE + TRAMPOLINE_REACHED), %esi

    movl    TRAMPOLINE_BASE + TRAMPOLINE_CONTEXT, %edi
    movl    TRAMPOLINE_BASE + TRAMPOLINE_ENTRY, %eax
    jmpl    *%eax

    .align 8
gdt:
    .quad   0
    .quad   0x00CF9A000000FFFF              /* 0x08: 32-bit code, flat */
    .quad   0x00CF92000000FFFF              /* 0x10: data, flat */
gdt_end:

gdt_pointer:
    .word   gdt_end - gdt - 1
    .long   AT(gdt)

    .org    TRAMPOLINE_DATA
    .long   0                               /* entry */
    .long   0                               /* context */
    .long   0                               /* reached */

    .global trampoline_end
trampoline_end:
