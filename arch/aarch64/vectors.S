/*
 * The AArch64 exception vector table.
 *
 * Sixteen entries, 128 bytes apart, in a 2 KB-aligned table whose address
 * goes in VBAR_EL1. All sixteen exist even though only four can currently
 * fire, because an exception arriving at an entry that was never written
 * lands on whatever bytes happen to be there and executes them.
 *
 * The four groups, in the order the architecture fixes:
 *
 *   0x000  current EL, SP_EL0     cannot happen: the kernel runs at EL1h
 *   0x200  current EL, SP_ELx     where every kernel fault lands today
 *   0x400  lower EL, AArch64      userland, from M4
 *   0x600  lower EL, AArch32      never: nothing in Kosmos is AArch32
 *
 * Each entry only makes room for the frame, saves x0 and x1, puts its own
 * index in x0 and branches. 128 bytes is not enough for the whole save
 * sequence, and the index has to be known before there is a free register
 * to hold it, which is why the stub saves before it computes.
 */

#include "trap.h"

.macro VECTOR index
    .align  7                       /* 2^7 = 128 bytes per entry */
    sub     sp, sp, #TF_SIZE
    stp     x0, x1, [sp, #TF_X0]
    mov     x0, #\index
    b       trap_entry
.endm

.section .text
.align  11                          /* 2^11 = 2 KB, required by VBAR_EL1 */
.global vectors
vectors:
    VECTOR  0                       /* current EL, SP_EL0: sync   */
    VECTOR  1                       /* current EL, SP_EL0: irq    */
    VECTOR  2                       /* current EL, SP_EL0: fiq    */
    VECTOR  3                       /* current EL, SP_EL0: serror */

    VECTOR  4                       /* current EL, SP_ELx: sync   */
    VECTOR  5                       /* current EL, SP_ELx: irq    */
    VECTOR  6                       /* current EL, SP_ELx: fiq    */
    VECTOR  7                       /* current EL, SP_ELx: serror */

    VECTOR  8                       /* lower EL, AArch64: sync    */
    VECTOR  9                       /* lower EL, AArch64: irq     */
    VECTOR  10                      /* lower EL, AArch64: fiq     */
    VECTOR  11                      /* lower EL, AArch64: serror  */

    VECTOR  12                      /* lower EL, AArch32: sync    */
    VECTOR  13                      /* lower EL, AArch32: irq     */
    VECTOR  14                      /* lower EL, AArch32: fiq     */
    VECTOR  15                      /* lower EL, AArch32: serror  */

/*
 * Entered with the frame already allocated, x0 and x1 already in it, and the
 * vector index live in x0.
 */
trap_entry:
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30,      [sp, #240]

    /* The sp the interrupted code had, which is this one before the frame
     * was carved out of it. */
    add     x1, sp, #TF_SIZE
    mrs     x2, elr_el1
    mrs     x3, spsr_el1
    mrs     x4, esr_el1
    mrs     x5, far_el1
    stp     x1, x2, [sp, #TF_SP]        /* sp, elr   */
    stp     x3, x4, [sp, #TF_SPSR]      /* spsr, esr */
    str     x5,     [sp, #TF_FAR]

    /* trap_handler(index, frame). x0 still holds the index. */
    mov     x1, sp
    bl      trap_handler

    /*
     * The handler may have moved elr, which is how an expected fault steps
     * past the instruction that caused it. Write both back before restoring
     * the general registers, because x0 and x1 are still needed as scratch.
     */
    ldp     x0, x1, [sp, #TF_ELR]       /* elr, spsr */
    msr     elr_el1, x0
    msr     spsr_el1, x1

    ldp     x2,  x3,  [sp, #16]
    ldp     x4,  x5,  [sp, #32]
    ldp     x6,  x7,  [sp, #48]
    ldp     x8,  x9,  [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30,      [sp, #240]

    ldp     x0, x1, [sp, #TF_X0]
    add     sp, sp, #TF_SIZE
    eret
