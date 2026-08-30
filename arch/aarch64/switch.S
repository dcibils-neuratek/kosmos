/*
 * The context switch.
 *
 * Cooperative: it is called from C, at a point the outgoing thread chose, so
 * the compiler has already spilled whatever caller-saved registers it cared
 * about. Only the callee-saved set has to survive, which is the same set
 * setjmp saves and for the same reason.
 *
 *   x19 to x28    the callee-saved general registers
 *   x29, x30      frame pointer and link register
 *   d8 to d15     the callee-saved halves of the FP registers
 *
 * The FP half is not optional and this is where it comes due. The kernel's
 * own C is built -mgeneral-regs-only and never touches them, but Lua runs on
 * a thread and its numbers are doubles. Switching away from a thread with a
 * live d8 and not saving it would corrupt a value with no trace of where it
 * happened.
 *
 * Two stacks move, not one. The thread runs on SP_EL0 and its exceptions are
 * taken on SP_EL1, so both belong to the thread and both are switched here.
 * That is what makes preemption possible later: when the switch happens
 * inside an interrupt handler, the incoming thread's exception stack already
 * holds the frame its own interrupt built, and the handler's epilogue pops
 * that one.
 *
 * A botched context switch corrupts registers in a way that surfaces five
 * functions later, which is why the layout is written out rather than
 * computed, and why there is a test asserting every saved register survives.
 */

#include "context.h"

.section .text
.global context_switch

/*
 * void context_switch(struct context *prev, struct context *next);
 *   x0 = where to save the outgoing thread
 *   x1 = where to load the incoming one from
 */
context_switch:
    /*
     * Mask interrupts for the duration, and record what they were.
     *
     * The swap below leaves SPSel flipped for three instructions at a time.
     * An interrupt arriving in that window would be taken on a stack pointer
     * that is mid-exchange. Saving the previous mask rather than
     * unconditionally re-enabling means this is correct when called from
     * somewhere that already held interrupts off, which IPC will.
     */
    mrs     x9, daif
    str     x9, [x0, #CTX_DAIF]
    msr     daifset, #3

    /* The general callee-saved set, straight into the outgoing context. */
    stp     x19, x20, [x0, #CTX_X19]
    stp     x21, x22, [x0, #CTX_X21]
    stp     x23, x24, [x0, #CTX_X23]
    stp     x25, x26, [x0, #CTX_X25]
    stp     x27, x28, [x0, #CTX_X27]
    stp     x29, x30, [x0, #CTX_X29]

    stp     d8,  d9,  [x0, #CTX_D8]
    stp     d10, d11, [x0, #CTX_D10]
    stp     d12, d13, [x0, #CTX_D12]
    stp     d14, d15, [x0, #CTX_D14]

    /*
     * Both stack pointers, and the awkward part.
     *
     * SP_EL1 cannot be named from EL1. Its system register encoding is an
     * EL2 one, so `mrs x10, sp_el1` here is not a privileged access that
     * traps, it is an undefined instruction: the fault arrives as EC 0x00,
     * "unknown reason", pointing at the mrs and explaining nothing. That is
     * how this was found.
     *
     * The only way to reach it from EL1 is to make it the current stack
     * pointer for a moment. Nothing between the two SPSel writes may touch
     * the stack, which is why these are register moves and nothing else.
     */
    mov     x9, sp                  /* SP_EL0: SPSel is clear */
    msr     spsel, #1
    mov     x10, sp                 /* now `sp` names SP_EL1 */
    msr     spsel, #0
    stp     x9, x10, [x0, #CTX_SP]

    /* From here nothing may touch the outgoing thread's stack again. */
    ldp     x9, x10, [x1, #CTX_SP]
    mov     sp, x9
    msr     spsel, #1
    mov     sp, x10
    msr     spsel, #0

    ldp     x19, x20, [x1, #CTX_X19]
    ldp     x21, x22, [x1, #CTX_X21]
    ldp     x23, x24, [x1, #CTX_X23]
    ldp     x25, x26, [x1, #CTX_X25]
    ldp     x27, x28, [x1, #CTX_X27]
    ldp     x29, x30, [x1, #CTX_X29]

    ldp     d8,  d9,  [x1, #CTX_D8]
    ldp     d10, d11, [x1, #CTX_D10]
    ldp     d12, d13, [x1, #CTX_D12]
    ldp     d14, d15, [x1, #CTX_D14]

    /* The mask the incoming thread was running under, not the outgoing one's. */
    ldr     x9, [x1, #CTX_DAIF]
    msr     daif, x9

    /*
     * Returns into the incoming thread, because x30 now holds where that
     * thread was when it last called this function. A brand new thread has
     * had x30 planted with thread_entry, which is how it starts running
     * without ever having been switched away from.
     */
    ret

/*
 * Where a thread begins. Never called: reached by the `ret` above, on a
 * context built by hand.
 *
 * x19 holds the function to run and x20 its argument, because those are the
 * first two slots of the saved context and so the easiest to plant.
 */
.global thread_entry
thread_entry:
    mov     x0, x20
    blr     x19

    /*
     * The function returned. A thread that runs off its own end has no
     * caller to return to, so it goes to thread_exit, which takes it off the
     * runqueue and switches away for good. Falling through here instead
     * would return into whatever x30 happened to hold.
     */
    b       thread_exit
