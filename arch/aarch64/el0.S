/*
 * Dropping to EL0.
 *
 * Called once per process, from the thread that will host it. It does not
 * return: the thread is at EL0 from here on, and every subsequent entry into
 * the kernel arrives as an exception, on SP_EL1.
 *
 * That split is what makes the earlier stack arrangement survive userland.
 * A kernel thread runs on SP_EL0 with its exceptions taken on SP_EL1; a
 * thread that has entered EL0 has given SP_EL0 to the process and does its
 * kernel-side work on SP_EL1 instead. Both are already saved and restored by
 * context_switch, so a process thread is preempted like any other.
 */

.section .text
.global enter_el0

/*
 * void enter_el0(uintptr_t entry, uintptr_t user_sp, unsigned long arg);
 *
 * `arg` is the only thing a process is told, and it arrives in x0. Everything
 * else it can reach is in its capability table, which is the point: what a
 * process has is what it was handed, and one word of configuration does not
 * change that.
 */
enter_el0:
    /*
     * Nothing may interrupt the transition.
     *
     * Between the SPSel write below and the eret, this runs at EL1 with
     * SPSel set, which is the state the vector table reserves for an
     * exception taken inside the handler. An interrupt arriving in that
     * window lands in the wrong quarter of the table and is reported as a
     * double fault, which is exactly what happened the first time this ran
     * long enough for the timer to fire.
     *
     * Nothing is lost by masking: the eret restores PSTATE from SPSR_EL1,
     * which is zero, so the process starts with interrupts enabled.
     */
    msr     daifset, #3

    msr     elr_el1, x0             /* where EL0 starts */

    /*
     * SP_EL0 can only be named while it is *not* the current stack pointer,
     * which is the mirror image of the rule for SP_EL1. A kernel thread runs
     * with SPSel clear, so `msr sp_el0, x1` there is not a privileged access
     * that traps, it is an undefined instruction: EC 0x00, pointing at the
     * msr and explaining nothing.
     *
     * Switching to SP_EL1 first makes it nameable, and it is where this
     * thread belongs from now on anyway: it is about to hand SP_EL0 to the
     * process, and every exception from EL0 arrives on SP_EL1. The C frame
     * left behind on the old stack is never returned to, because this
     * function does not return.
     */
    msr     spsel, #1
    msr     sp_el0, x1              /* the process's own stack */

    /*
     * SPSR for the eret: M[3:0] = 0b0000, meaning EL0t, and DAIF all clear so
     * the process runs with interrupts enabled. A process with interrupts
     * masked could not be preempted, which would hand the machine to the
     * first one that decided not to make a syscall.
     */
    msr     spsr_el1, xzr

    /*
     * Nothing is left in a register on purpose. Whatever the kernel had in
     * x0 to x30 would be visible to EL0, and a kernel pointer handed over by
     * accident is a leak that no page table catches.
     */
    /* x2 holds the boot word and has to survive the zeroing, so it is moved
     * into place first and x0 is not cleared. */
    mov     x0,  x2
    mov     x1,  xzr
    mov     x2,  xzr
    mov     x3,  xzr
    mov     x4,  xzr
    mov     x5,  xzr
    mov     x6,  xzr
    mov     x7,  xzr
    mov     x8,  xzr
    mov     x9,  xzr
    mov     x10, xzr
    mov     x11, xzr
    mov     x12, xzr
    mov     x13, xzr
    mov     x14, xzr
    mov     x15, xzr
    mov     x16, xzr
    mov     x17, xzr
    mov     x18, xzr
    mov     x19, xzr
    mov     x20, xzr
    mov     x21, xzr
    mov     x22, xzr
    mov     x23, xzr
    mov     x24, xzr
    mov     x25, xzr
    mov     x26, xzr
    mov     x27, xzr
    mov     x28, xzr
    mov     x29, xzr
    mov     x30, xzr

    eret
