/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef ARCH_X86_64_CONTEXT_H
#define ARCH_X86_64_CONTEXT_H

/*
 * What a switched-out thread consists of.
 *
 * Included from both C and assembly, so everything above the __ASSEMBLER__
 * guard is plain preprocessor and the _Static_asserts below are what keep
 * the offsets from drifting away from switch.S. Getting one of them wrong
 * corrupts a register in a way that surfaces five functions later.
 *
 * **Much shorter than the AArch64 one, and the reason is the ABI rather
 * than the architecture.** System V on AMD64 makes rbx, rbp and r12-r15
 * callee-saved and *every* XMM register caller-saved - so a thread that
 * called this function has already had the compiler spill anything else it
 * cared about, floating point included. AAPCS64 keeps d8-d15 across a call,
 * which is why the ARM context has to think about FP even in the
 * cooperative case and this one does not.
 */

#define CTX_RBX     0
#define CTX_RBP     8
#define CTX_R12     16
#define CTX_R13     24
#define CTX_R14     32
#define CTX_R15     40
#define CTX_RSP     48
#define CTX_RIP     56
#define CTX_RFLAGS  64
#define CTX_KSTACK  72
#define CTX_SIZE    80

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>

struct context {
    uint64_t rbx;
    uint64_t rbp;       /* frame pointer */
    uint64_t r12, r13, r14, r15;

    /*
     * The thread's stack, as it will be when the thread resumes - above the
     * return address rather than at it, so that restoring is a push and a
     * `ret` and the two halves are exact mirrors.
     */
    uint64_t rsp;

    /*
     * Where the thread resumes. There is no `mov` to rip on x86, so this is
     * pushed onto the incoming thread's own stack and reached with `ret` -
     * putting it back in the slot the outgoing `call` had it in.
     */
    uint64_t rip;

    /*
     * The flags this thread was running under, which is where the interrupt
     * mask lives - the counterpart of AArch64's `daif`. Saved and restored
     * rather than assumed, so a switch is correct whether or not the caller
     * had interrupts enabled; IPC will call it from places that already
     * hold them masked, and assuming would silently re-enable interrupts
     * halfway through a critical section.
     *
     * The direction flag is in here too, which matters more than it looks:
     * `rep movs` runs backwards when DF is set, and the ABI says a function
     * is entered with it clear. A thread preempted between `std` and `cld`
     * would otherwise hand the next thread a backwards `memcpy`.
     */
    uint64_t rflags;

    /*
     * The stack an entry from ring 3 lands on, which is this thread's and
     * not the switch's.
     *
     * AArch64 has SP_EL1 in exactly this slot and for exactly this reason,
     * and the hardware selects it on an exception from EL0. x86 reads it
     * out of the TSS instead, and the TSS is one structure for the whole
     * machine - so `context_switch` writes this field into it, which is the
     * same store the ARM switch makes and lands somewhere else.
     *
     * It is not loaded back into any register, so nothing in `switch.S`
     * saves it: it is written once when the thread is built and read on
     * every switch *to* the thread.
     */
    uint64_t kernel_stack;
};

_Static_assert(sizeof(struct context) == CTX_SIZE, "context size vs switch.S");
_Static_assert(offsetof(struct context, rbx)    == CTX_RBX,    "CTX_RBX");
_Static_assert(offsetof(struct context, rbp)    == CTX_RBP,    "CTX_RBP");
_Static_assert(offsetof(struct context, r12)    == CTX_R12,    "CTX_R12");
_Static_assert(offsetof(struct context, r13)    == CTX_R13,    "CTX_R13");
_Static_assert(offsetof(struct context, r14)    == CTX_R14,    "CTX_R14");
_Static_assert(offsetof(struct context, r15)    == CTX_R15,    "CTX_R15");
_Static_assert(offsetof(struct context, rsp)    == CTX_RSP,    "CTX_RSP");
_Static_assert(offsetof(struct context, rip)    == CTX_RIP,    "CTX_RIP");
_Static_assert(offsetof(struct context, rflags) == CTX_RFLAGS, "CTX_RFLAGS");
_Static_assert(offsetof(struct context, kernel_stack) == CTX_KSTACK,
               "CTX_KSTACK");

/*
 * There is no XMM state in here yet, and that is a gap rather than a
 * difference.
 *
 * A thread that *calls* this function needs none saved - the ABI already
 * spilled it. A thread stopped by the timer between two instructions does,
 * exactly as on ARM, and for now nothing preempts anything here.
 *
 * When it arrives it takes the shape `arch/aarch64/fp.c` already settled
 * on, because the mechanism exists on both machines: the switch disarms the
 * registers, the first instruction that touches them faults, and the
 * handler moves them from the previous owner to the new one. CR0.TS is the
 * disarm and the fault is vector 7 - the one `trap.c` already calls "device
 * not available", which is what it was named for in 1985. A 512-byte FXSAVE
 * area joins this struct then, along with the owner tracking and the
 * `fp_forget` that keeps a dead thread from being saved into.
 *
 * It is written down rather than half-built: a field nothing reads is
 * indistinguishable from a bug, and the pieces only make sense together.
 */

/* Saves into prev, loads from next, and returns inside next. */
void context_switch(struct context *prev, struct context *next);

/* Where a hand-built context starts. Never called directly. */
void thread_entry(void);

/*
 * A context that has never run, so the first `ret` in `context_switch`
 * starts it.
 *
 * **This exists so that `kernel/thread.c` does not name a register.** It
 * planted seven AArch64 registers itself until the second architecture made
 * the difference between "no assembly in the kernel" and "portable kernel"
 * something that mattered.
 *
 * rbx and r12 because they are the first two slots of the saved context and
 * so the easiest to plant, and `thread_entry` reads them from there. 0x202
 * is IF set and bit 1, which reads as 1 always: a thread that could not be
 * preempted would keep the machine the moment it chose not to yield.
 */
static inline void context_init(struct context *ctx, void (*entry)(void *),
                                void *arg, void *stack_top,
                                void *exception_top)
{
    ctx->rbx = (uint64_t)(uintptr_t)entry;
    ctx->r12 = (uint64_t)(uintptr_t)arg;
    ctx->rip = (uint64_t)(uintptr_t)thread_entry;
    ctx->rsp = (uint64_t)(uintptr_t)stack_top;
    ctx->rflags = 0x202;
    ctx->kernel_stack = (uint64_t)(uintptr_t)exception_top;
}

/*
 * Hands this thread to a process and does not return.
 *
 * `enter_ring3` was its name here and `enter_el0` on ARM, and `process.c`
 * declared one of them by hand - the one place the kernel said out loud
 * which architecture it was on. What the caller means is "run at the
 * unprivileged level", and every machine has one under a different name.
 */
void enter_user(uintptr_t entry, uintptr_t user_sp, unsigned long arg);

#endif /* !__ASSEMBLER__ */

#endif /* ARCH_X86_64_CONTEXT_H */
