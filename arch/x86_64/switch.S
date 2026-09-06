/*
 * The context switch, x86-64.
 *
 * Cooperative: called from C, at a point the outgoing thread chose, so the
 * compiler has already spilled whatever caller-saved registers it cared
 * about. Only System V's callee-saved set has to survive:
 *
 *   rbx, rbp, r12-r15    the callee-saved general registers
 *   rsp                  the thread's stack
 *   rflags               and with it the interrupt mask and DF
 *
 * **No floating point here, and unlike the ARM side that is not a lazy
 * scheme - it is the ABI.** Every XMM register is caller-saved on AMD64, so
 * a thread that called this function has nothing left in them worth
 * keeping. AAPCS64 preserves d8-d15 across a call, which is why the ARM
 * switch has to have an answer for FP and this one does not. The answer
 * both of them will need is for *preempted* threads, and `context.h` says
 * what shape it takes when it arrives.
 *
 * One stack moves, not two. AArch64 has SP_EL0 and SP_EL1 and a thread owns
 * both, so its switch exchanges a pair and has to record which was
 * selected. x86 has one stack pointer per privilege level *transition*: an
 * interrupt taken in ring 0 keeps using the current stack, and one taken in
 * ring 3 loads RSP0 out of the TSS. So the kernel stack a thread's
 * exceptions land on is a field in the TSS rather than a register in here,
 * and it is set when the thread is scheduled rather than saved when it is
 * switched away.
 *
 * A botched context switch corrupts registers in a way that surfaces five
 * functions later, which is why the layout is written out rather than
 * computed.
 */

#include "context.h"

.section .text
.global context_switch

/*
 * void context_switch(struct context *prev, struct context *next);
 *   rdi = where to save the outgoing thread
 *   rsi = where to load the incoming one from
 */
context_switch:
    /*
     * The flags first, and then interrupts off for the duration.
     *
     * `pushfq` needs a stack and this is the last moment there is a settled
     * one, since rsp is exchanged below. Recording the previous state
     * rather than unconditionally enabling means this is correct when
     * called from somewhere that already held interrupts off, which IPC
     * will.
     */
    pushfq
    popq    %rax
    movq    %rax, CTX_RFLAGS(%rdi)
    cli

    movq    %rbx, CTX_RBX(%rdi)
    movq    %rbp, CTX_RBP(%rdi)
    movq    %r12, CTX_R12(%rdi)
    movq    %r13, CTX_R13(%rdi)
    movq    %r14, CTX_R14(%rdi)
    movq    %r15, CTX_R15(%rdi)

    /*
     * Where this thread resumes is the return address the `call` left on
     * top of the stack, and the stack it resumes on is the one just above
     * it. Splitting them here is what lets the restore below be a push and
     * a `ret`: the address goes back into the slot it came out of.
     */
    movq    (%rsp), %rax
    movq    %rax, CTX_RIP(%rdi)
    leaq    8(%rsp), %rax
    movq    %rax, CTX_RSP(%rdi)

    /* From here nothing may touch the outgoing thread's stack again. */
    movq    CTX_RBX(%rsi), %rbx
    movq    CTX_RBP(%rsi), %rbp
    movq    CTX_R12(%rsi), %r12
    movq    CTX_R13(%rsi), %r13
    movq    CTX_R14(%rsi), %r14
    movq    CTX_R15(%rsi), %r15

    movq    CTX_RSP(%rsi), %rsp
    pushq   CTX_RIP(%rsi)

    /*
     * The incoming thread's flags, not the outgoing one's - and this is the
     * instruction that may switch interrupts back on, so nothing that
     * matters happens after it.
     *
     * No serialising instruction is needed around any of this. AArch64
     * needs an `isb` after writing CPACR because a control register change
     * is not in force for instructions already fetched; `popfq` is not that
     * kind of write, and x86 orders it against what follows on its own.
     */
    pushq   CTX_RFLAGS(%rsi)
    popfq

    /*
     * Returns into the incoming thread, because the address now on top of
     * its stack is where that thread was when it last called this function.
     * A brand new thread has had `rip` planted with thread_entry, which is
     * how it starts running without ever having been switched away from.
     */
    ret

/*
 * Where a thread begins. Never called: reached by the `ret` above, on a
 * context built by hand.
 *
 * rbx holds the function to run and r12 its argument, because those are the
 * first two slots of the saved context and so the easiest to plant.
 */
.global thread_entry
thread_entry:
    /*
     * The stack is 16-byte aligned here rather than at the usual offset,
     * because nothing called us: `ret` popped the address thread_create
     * planted and left rsp where the thread's stack began. System V wants
     * rsp+8 aligned to 16 at the *entry* of a function, which is what a
     * `call` produces - so the `call` below is what puts it right, and the
     * stack this runs on has to start 16-aligned for that to hold.
     *
     * A misalignment here is invisible until something in the callee uses
     * an aligned SSE store, and then it is a general protection fault a
     * long way from the cause.
     */
    movq    %r12, %rdi
    call    *%rbx

    /*
     * The function returned. A thread that runs off its own end has no
     * caller to return to, so it goes to thread_exit, which takes it off
     * the runqueue and switches away for good. Falling through here instead
     * would return into whatever the stack happened to hold.
     */
    jmp     thread_exit
