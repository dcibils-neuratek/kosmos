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
 *   v0 to v31     and then all of them, plus fpcr and fpsr, because this is
 *                 also where a *preempted* thread is switched away
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

    /*
     * No FP here any more, and that is the point.
     *
     * This used to write out all thirty-two Q registers and read them back,
     * on every switch, for every thread - and it cost 36% of a switch and
     * 17% of an IPC round trip to preserve registers most threads never
     * touch. The correctness argument for saving them was right and is
     * unchanged; what was wrong was doing it here, eagerly.
     *
     * Instead the epilogue disarms FP for EL0 and the first floating-point
     * instruction the incoming thread executes traps into `fp_fault`, which
     * saves whoever owned the registers and loads whoever wants them. See
     * `fp.c`.
     */

    /*
     * Both stack pointers, and the awkward part.
     *
     * SP_EL1 cannot be named from EL1. Its system register encoding is an
     * EL2 one, so `mrs x10, sp_el1` here is not a privileged access that
     * traps, it is an undefined instruction: the fault arrives as EC 0x00,
     * "unknown reason", pointing at the mrs and explaining nothing.
     *
     * The only way to reach either from EL1 is to make it the current stack
     * pointer for a moment. Nothing between the SPSel writes may touch the
     * stack, which is why these are register moves and nothing else, and why
     * this whole function uses no stack of its own.
     *
     * Which one is current on entry is not fixed and must not be assumed. A
     * thread that yields arrives on SP_EL0; a thread being preempted arrives
     * from the vector's epilogue, where taking the exception already set
     * SPSel to 1. Assuming SP_EL0 wrote SP_EL1 into both slots and left the
     * wrong one selected, which produced a jump to an address that was
     * never code.
     */
    mrs     x11, spsel
    and     x11, x11, #1
    str     x11, [x0, #CTX_SPSEL]

    msr     spsel, #0
    mov     x9, sp                  /* SP_EL0, whichever we arrived on */
    msr     spsel, #1
    mov     x10, sp                 /* SP_EL1 */
    stp     x9, x10, [x0, #CTX_SP]

    /* From here nothing may touch the outgoing thread's stack again. */
    ldp     x9, x10, [x1, #CTX_SP]
    msr     spsel, #0
    mov     sp, x9
    msr     spsel, #1
    mov     sp, x10

    /* And select whichever the incoming thread was using. */
    ldr     x11, [x1, #CTX_SPSEL]
    msr     spsel, x11

    ldp     x19, x20, [x1, #CTX_X19]
    ldp     x21, x22, [x1, #CTX_X21]
    ldp     x23, x24, [x1, #CTX_X23]
    ldp     x25, x26, [x1, #CTX_X25]
    ldp     x27, x28, [x1, #CTX_X27]
    ldp     x29, x30, [x1, #CTX_X29]

    /*
     * FP off for EL0, for whichever thread is about to run.
     *
     * 0b00 in CPACR_EL1.FPEN traps both levels, and both is the answer
     * rather than EL0 alone.
     *
     * Trapping only EL0 was tried and three tests said no. Kernel threads
     * run at EL1, so their registers would be neither saved by this switch
     * nor faulted in by anything - two of them holding a value across a
     * preemption lose it, which is the bug the eager save was added to fix
     * in the first place. And `longjmp` writes d8-d15 from its buffer, so a
     * kernel thread returning through one would overwrite whatever EL0
     * thread currently owns those registers.
     *
     * Uniform is simpler than either: whoever touches an FP register first
     * after a switch faults, and the fault does not care which level it came
     * from.
     *
     * The `isb` is not decoration. CPACR is a control register and the
     * change has to be in force before the first instruction of the incoming
     * thread is fetched; without it the trap can be skipped for exactly the
     * instruction it was armed for.
     */
    mov     x9, #(0 << 20)
    msr     cpacr_el1, x9
    isb

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
    /*
     * Arrives already on SP_EL0 with interrupts enabled, because
     * context_switch restored both from this thread's context and
     * thread_create left both at zero. That matters most when a thread is
     * started from inside the vector's epilogue, where SPSel is 1 and the
     * eret that would have cleared it is never reached.
     */
    mov     x0, x20
    blr     x19

    /*
     * The function returned. A thread that runs off its own end has no
     * caller to return to, so it goes to thread_exit, which takes it off the
     * runqueue and switches away for good. Falling through here instead
     * would return into whatever x30 happened to hold.
     */
    b       thread_exit
