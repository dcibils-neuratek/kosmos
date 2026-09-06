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
#define CTX_FX      80      /* 512 bytes, and 16-byte aligned or it faults */
#define CTX_SIZE    592

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

    /*
     * The x87 and SSE register file, saved on every switch.
     *
     * **This was deliberately absent until userland arrived, and userland
     * is what changed the answer.** A thread that *calls* the switch needs
     * none of this saved: System V makes every XMM register caller-saved,
     * so the compiler already spilled whatever it cared about. A thread
     * stopped by the timer between two instructions called nothing, and
     * Lua's numbers are doubles - so a preempted process would resume
     * mid-expression with the next process's values, which is a wrong
     * answer and never a crash. AArch64 has the identical bug in its
     * history and `test_fp_survives_a_preemption` catches it in about two
     * spins in sixty.
     *
     * **Saved eagerly, and that is the order rather than the destination.**
     * The ARM side did exactly this, measured what it cost - 36% of a
     * context switch and 17% of an IPC round trip, for threads that mostly
     * never touch an FP register - and then made it lazy: the switch
     * disarms FP, the first instruction that wants it faults, and the
     * handler moves the file. The whole mechanism exists here too, and
     * `fp.c` writes down what it is: CR0.TS, vector 7, FXSAVE. What does
     * not exist yet is the measurement, and this system does not push work
     * down without one. Correct first, and 36% of a number nobody has taken
     * is not an argument.
     *
     * 512 bytes, aligned to 16, because that is what FXSAVE requires of its
     * operand - and an unaligned one is a general protection fault rather
     * than a slow path. The alignment on this member is what gives the
     * whole struct its alignment, which is what makes the offset above
     * true for every thread.
     */
    uint8_t fx[512] __attribute__((aligned(16)));
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
_Static_assert(offsetof(struct context, fx) == CTX_FX, "CTX_FX");
_Static_assert(offsetof(struct context, fx) % 16 == 0, "FXSAVE wants 16");

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

    /*
     * A clean floating-point state, written out rather than left zero.
     *
     * FXRSTOR of an all-zero area does not fault - nothing reserved is set
     * - and it is still wrong: MXCSR of zero means every SIMD exception
     * *unmasked*, so a divide by zero in a process raises #XF instead of
     * producing an infinity, which is not what any language expects. 0x1F80
     * is the reset value, with all six masked, and 0x037F is the x87
     * control word's.
     *
     * Copied from a template captured at boot would be the other way to do
     * it, and it is worse: the template would hold whatever registers some
     * thread had live, and a new process would start with another's
     * numbers in it.
     */
    {
        unsigned i;

        for (i = 0; i < sizeof(ctx->fx); i++) {
            ctx->fx[i] = 0;
        }

        ctx->fx[0] = 0x7f;      /* FCW, at offset 0  */
        ctx->fx[1] = 0x03;
        ctx->fx[24] = 0x80;     /* MXCSR, at offset 24 */
        ctx->fx[25] = 0x1f;
    }
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
