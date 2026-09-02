/*
 * The FP and SIMD register file, saved and restored on demand.
 *
 * These used to be the middle of `context_switch` and ran on every switch.
 * They run now only when a thread actually touches an FP register after
 * being scheduled, which for most threads is never: the kernel's own C is
 * built -mgeneral-regs-only and cannot emit an FP instruction at all.
 *
 * Whole 128-bit Q registers, not the low halves. Userland is not built
 * -mgeneral-regs-only and the compiler is free to use the upper halves, so
 * saving only what Lua's arithmetic obviously needs would be the same bug
 * one layer down. `fpcr` and `fpsr` go with them: rounding mode and the
 * sticky exception flags are part of what a thread was doing.
 */

#include "context.h"

.section .text

/*
 * void fp_save(struct context *ctx);   x0 = where to write
 */
.global fp_save
fp_save:
    add     x1, x0, #CTX_Q0
    stp     q0,  q1,  [x1, #0]
    stp     q2,  q3,  [x1, #32]
    stp     q4,  q5,  [x1, #64]
    stp     q6,  q7,  [x1, #96]
    stp     q8,  q9,  [x1, #128]
    stp     q10, q11, [x1, #160]
    stp     q12, q13, [x1, #192]
    stp     q14, q15, [x1, #224]
    stp     q16, q17, [x1, #256]
    stp     q18, q19, [x1, #288]
    stp     q20, q21, [x1, #320]
    stp     q22, q23, [x1, #352]
    stp     q24, q25, [x1, #384]
    stp     q26, q27, [x1, #416]
    stp     q28, q29, [x1, #448]
    stp     q30, q31, [x1, #480]

    mrs     x2, fpcr
    mrs     x3, fpsr
    add     x1, x0, #CTX_FPCR
    stp     x2, x3, [x1]
    ret

/*
 * void fp_restore(struct context *ctx);   x0 = where to read
 */
.global fp_restore
fp_restore:
    add     x1, x0, #CTX_FPCR
    ldp     x2, x3, [x1]
    msr     fpcr, x2
    msr     fpsr, x3

    add     x1, x0, #CTX_Q0
    ldp     q0,  q1,  [x1, #0]
    ldp     q2,  q3,  [x1, #32]
    ldp     q4,  q5,  [x1, #64]
    ldp     q6,  q7,  [x1, #96]
    ldp     q8,  q9,  [x1, #128]
    ldp     q10, q11, [x1, #160]
    ldp     q12, q13, [x1, #192]
    ldp     q14, q15, [x1, #224]
    ldp     q16, q17, [x1, #256]
    ldp     q18, q19, [x1, #288]
    ldp     q20, q21, [x1, #320]
    ldp     q22, q23, [x1, #352]
    ldp     q24, q25, [x1, #384]
    ldp     q26, q27, [x1, #416]
    ldp     q28, q29, [x1, #448]
    ldp     q30, q31, [x1, #480]
    ret
