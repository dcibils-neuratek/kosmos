/*
 * setjmp and longjmp for AArch64.
 *
 * Saves and restores exactly the AAPCS64 callee-saved set. Anything else is
 * the caller's own business: a caller-saved register is already spilled by
 * whoever cared about it, and restoring one would put back a stale value.
 *
 * The layout has to match jmp_buf in setjmp.h. There is no _Static_assert
 * reaching across into assembly, so the offsets are written out here rather
 * than hidden behind names that could drift.
 *
 *    0  x19   16  x21   32  x23   48  x25   64  x27   80  x29   96  sp
 *    8  x20   24  x22   40  x24   56  x26   72  x28   88  x30  104  (pad)
 *  112  d8   128  d10  144  d12  160  d14
 *  120  d9   136  d11  152  d13  168  d15
 */

.section .text
.global setjmp
.global longjmp

setjmp:
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]

    /* sp cannot be an operand of stp, so it goes through a scratch. x1 is
     * caller-saved, so using it here costs nothing. */
    mov     x1, sp
    str     x1, [x0, #96]

    stp     d8,  d9,  [x0, #112]
    stp     d10, d11, [x0, #128]
    stp     d12, d13, [x0, #144]
    stp     d14, d15, [x0, #160]

    mov     w0, #0
    ret

longjmp:
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]

    ldr     x2, [x0, #96]
    mov     sp, x2

    ldp     d8,  d9,  [x0, #112]
    ldp     d10, d11, [x0, #128]
    ldp     d12, d13, [x0, #144]
    ldp     d14, d15, [x0, #160]

    /*
     * longjmp(env, 0) has to make setjmp return 1, or the caller cannot tell
     * a jump from the original call.
     *
     * csinc w0, w1, wzr, ne:  w1 != 0 ? w1 : wzr + 1
     */
    cmp     w1, #0
    csinc   w0, w1, wzr, ne

    /* x30 was restored above and holds the instruction after the setjmp
     * call, so this returns there rather than to whoever called longjmp. */
    ret
