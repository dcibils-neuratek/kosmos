/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Who owns the floating-point registers, and how they change hands.
 *
 * The registers are a resource one thread holds at a time. Rather than
 * moving them on every context switch - which is correct and costs 36% of a
 * switch for threads that never use them - the switch turns FP off for EL0
 * and leaves them wherever they are. The next thread to execute a floating
 * point instruction traps here, and only then does anything move.
 *
 * A thread that uses FP pays one fault per time slice. A thread that does
 * not pays nothing at all, and most do not: the kernel's own C cannot emit
 * an FP instruction, so every kernel thread is in the second group.
 *
 * `owner` is per-CPU state in everything but name. `CLAUDE.md` forbids loose
 * mutable globals for exactly this reason and this is one until there is a
 * second core to give it to; when SMP arrives it belongs beside the
 * runqueue in the per-CPU structure, and the FP registers of a thread that
 * last ran on another core have to be recalled from that core rather than
 * assumed present. That is written here because it is the kind of thing
 * that is obvious now and invisible later.
 */

#include <stddef.h>
#include <stdint.h>

#include "context.h"
#include "cpu.h"

#include "thread.h"

/* Implemented in fp.S. */
void fp_save(struct context *ctx);
void fp_restore(struct context *ctx);

/* The thread whose values are in the registers right now, or none. */
static struct thread *owner;

static inline void fpen_allow(void)
{
    /* 0b11: no trap at either level. */
    __asm__ volatile("msr cpacr_el1, %0; isb" :: "r"(3UL << 20) : "memory");
}

static inline void fpen_trap(void)
{
    /* 0b00: both levels trap. */
    __asm__ volatile("msr cpacr_el1, %0; isb" :: "r"(0UL << 20) : "memory");
}

/*
 * A thread at EL0 touched an FP register while they were disarmed.
 *
 * Not an error: it is how a thread asks for them. The registers still hold
 * whatever the last owner left, so those go back to their owner's context
 * before this thread's are loaded over the top.
 *
 * The order matters and is the whole of the correctness argument. Saving
 * after loading would write the new thread's values into the old thread's
 * context; saving a thread that is no longer alive would write into a
 * recycled slot, which is why `fp_forget` exists and why the thread code
 * calls it.
 */
void fp_fault(void)
{
    struct thread *self = thread_current();

    if (self == NULL) {
        /*
         * Before there are threads - early boot, and the fault machinery in
         * `trap.c`, which runs `setjmp` with no thread of its own. There is
         * nobody to attribute the registers to, so simply allow them and
         * drop the ownership: the next real thread to fault will save
         * nothing, which is correct, because nothing worth saving belongs to
         * anyone.
         */
        owner = NULL;
        fpen_allow();
        return;
    }

    if (owner == self) {
        /*
         * Already the owner and still trapped, which means the switch
         * disarmed FP and nothing else has wanted them since. Nothing to
         * move: just arm them again.
         */
        fpen_allow();
        return;
    }

    fpen_allow();               /* the kernel has to touch them to move them */

    if (owner != NULL) {
        fp_save(&owner->ctx);
    }

    fp_restore(&self->ctx);
    owner = self;
}

/*
 * A thread is going away, and its registers must not be written into a slot
 * that is about to belong to somebody else.
 *
 * Called when a thread exits or its slot is recycled. Without it the next
 * fault would save into a dead thread's context - which is harmless while
 * the slot is unused and silent corruption the moment it is reused.
 */
void fp_forget(struct thread *t)
{
    if (owner == t) {
        owner = NULL;
    }
}

/*
 * Whether a thread currently holds the registers. For the tests, which
 * otherwise could not tell a lazy save that works from one that is quietly
 * saving everything every time.
 */
bool fp_owned_by(const struct thread *t)
{
    return owner == t;
}

/* Used at boot and by anything that needs a known state. */
void fp_reset(void)
{
    owner = NULL;
    fpen_trap();
}
