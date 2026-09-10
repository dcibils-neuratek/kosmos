/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The exception stubs.
 *
 * AArch64 has sixteen entries in a table, four per exception class, and the
 * handler works out what happened by reading ESR_EL1. x86 has 256 separate
 * vectors and the processor tells you which one only by *which entry it
 * jumped through* - so there has to be a stub per vector whose whole job is
 * to write the number down.
 *
 * **Some vectors push an error code and some do not**, which is the detail
 * that makes this file look repetitive rather than be a loop. The stack the
 * handler sees has to have the same shape either way, so the ones that push
 * nothing push a zero in its place. Getting this wrong does not fault: it
 * misaligns every field after it, and the handler reports a plausible wrong
 * address for a fault that happened somewhere else.
 *
 * Which vectors carry a code is not a pattern to derive - it is a list in
 * the manual: 8, 10, 11, 12, 13, 14, 17, 21, 29 and 30.
 * Intel SDM volume 3, table 6-1.
 */

.section .text

.macro STUB_NOCODE vector
.global isr\vector
isr\vector:
    pushq   $0                      /* the error code this one does not have */
    pushq   $\vector
    jmp     isr_common
.endm

.macro STUB_CODE vector
.global isr\vector
isr\vector:
    pushq   $\vector                /* the processor pushed the code already */
    jmp     isr_common
.endm

    STUB_NOCODE 0                   /* divide error */
    STUB_NOCODE 1                   /* debug */
    STUB_NOCODE 2                   /* NMI */
    STUB_NOCODE 3                   /* breakpoint */
    STUB_NOCODE 4                   /* overflow */
    STUB_NOCODE 5                   /* bound range */
    STUB_NOCODE 6                   /* invalid opcode */
    STUB_NOCODE 7                   /* device not available */
    STUB_CODE   8                   /* double fault */
    STUB_NOCODE 9
    STUB_CODE   10                  /* invalid TSS */
    STUB_CODE   11                  /* segment not present */
    STUB_CODE   12                  /* stack-segment fault */
    STUB_CODE   13                  /* general protection */
    STUB_CODE   14                  /* page fault */
    STUB_NOCODE 15
    STUB_NOCODE 16                  /* x87 floating point */
    STUB_CODE   17                  /* alignment check */
    STUB_NOCODE 18                  /* machine check */
    STUB_NOCODE 19                  /* SIMD floating point */
    STUB_NOCODE 20                  /* virtualisation */
    STUB_CODE   21                  /* control protection */
    STUB_NOCODE 22
    STUB_NOCODE 23
    STUB_NOCODE 24
    STUB_NOCODE 25
    STUB_NOCODE 26
    STUB_NOCODE 27
    STUB_NOCODE 28
    STUB_CODE   29                  /* VMM communication */
    STUB_CODE   30                  /* security */
    STUB_NOCODE 31

/*
 * And the hardware interrupts, 32 upward, which the PIC was remapped to.
 *
 * No error code is ever pushed for these, so they all take the NOCODE form
 * and the frame the handler sees is the same shape as an exception's. That
 * sameness is the point: one `isr_common`, one `struct trapframe`, and
 * `trap_handle` decides what it is looking at from the vector number.
 */
    STUB_NOCODE 32                  /* the timer */
    STUB_NOCODE 33                  /* keyboard */
    STUB_NOCODE 34
    STUB_NOCODE 35
    STUB_NOCODE 36
    STUB_NOCODE 37
    STUB_NOCODE 38
    STUB_NOCODE 39
    STUB_NOCODE 40
    STUB_NOCODE 41
    STUB_NOCODE 42
    STUB_NOCODE 43
    STUB_NOCODE 44
    STUB_NOCODE 45
    STUB_NOCODE 46
    STUB_NOCODE 47

/*
 * And eight more lines than the 8259 pair could offer, plus room for the
 * local APIC's spurious vector.
 *
 * An I/O APIC has twenty-four inputs where the two 8259s between them had
 * fifteen, so a machine driving the APIC can route interrupts this IDT had
 * no gate for. **63 is the spurious vector** and is chosen rather than
 * convenient: the local APIC's spurious-interrupt register wants a vector
 * whose low four bits are set on some processors, and 0x3F is the highest
 * one this table reaches.
 */
    STUB_NOCODE 48
    STUB_NOCODE 49
    STUB_NOCODE 50
    STUB_NOCODE 51
    STUB_NOCODE 52
    STUB_NOCODE 53
    STUB_NOCODE 54
    STUB_NOCODE 55
    STUB_NOCODE 56
    STUB_NOCODE 57
    STUB_NOCODE 58
    STUB_NOCODE 59
    STUB_NOCODE 60
    STUB_NOCODE 61
    STUB_NOCODE 62
    STUB_NOCODE 63

/*
 * Everything the handler is allowed to look at, and everything it must put
 * back.
 *
 * The order here is the order `struct trapframe` declares, backwards -
 * `pushq` grows down, so the last thing pushed is the first field. Getting
 * the two out of step is a silent bug: the C sees `rax` where `r15` is.
 */
isr_common:
    /*
     * **From ring 3, this core's GS first**, before a single push. The low
     * two bits of the saved CS are the privilege level the interrupt came
     * from, and after the two words the stub pushed, that CS is 24 bytes up.
     * From ring 0 GS is already the kernel's, and swapping it would hand the
     * handler the process's.
     */
    testb   $3, 24(%rsp)
    jz      1f
    swapgs
1:
    pushq   %r15
    pushq   %r14
    pushq   %r13
    pushq   %r12
    pushq   %r11
    pushq   %r10
    pushq   %r9
    pushq   %r8
    pushq   %rbp
    pushq   %rdi
    pushq   %rsi
    pushq   %rdx
    pushq   %rcx
    pushq   %rbx
    pushq   %rax

    /* The frame itself as the argument. The System V ABI puts the first
     * one in rdi, and rsp is pointing at exactly what C wants to see. */
    movq    %rsp, %rdi
    call    trap_handle

    /*
     * Preemption happens here and nowhere else, exactly as it does in
     * `arch/aarch64/vectors.S`, and for exactly the same reason.
     *
     * It cannot be done inside `trap_handle`, because a context switch
     * moves rsp and everything below this point reads the frame at rsp.
     * That frame has to belong to whichever thread is about to be resumed.
     *
     * The mechanism falls out of that. The switch saves rsp pointing into
     * this function's own frame on the outgoing thread's kernel stack and
     * loads the incoming thread's, which points into *its* copy of this
     * same function, from when it was preempted. So the pops and the
     * `iretq` below run on the incoming thread's frame, and the outgoing
     * thread's stays untouched on its own stack until it is scheduled
     * again.
     *
     * Control may not come back from this call at all: if the incoming
     * thread has never run, the switch returns into `thread_entry` instead
     * and everything below is simply not executed for it, which is correct
     * because it has no frame to restore.
     *
     * **Leaving it out is a machine that ticks and never switches.**
     * `thread_tick` only records that the policy wants a different thread;
     * this is what acts on it. What that looked like: a boot that reached a
     * prompt, ran a program from the command line perfectly, counted its
     * ticks correctly - and ignored everything typed at it, because the
     * console server was never scheduled to notice.
     */
    call    thread_preempt_if_needed

    popq    %rax
    popq    %rbx
    popq    %rcx
    popq    %rdx
    popq    %rsi
    popq    %rdi
    popq    %rbp
    popq    %r8
    popq    %r9
    popq    %r10
    popq    %r11
    popq    %r12
    popq    %r13
    popq    %r14
    popq    %r15

    /*
     * And back, by the same test on the frame being returned to - which
     * after a switch is the incoming thread's, so a kernel thread resumed
     * here keeps the kernel's GS and a process gets its own.
     */
    testb   $3, 24(%rsp)
    jz      2f
    swapgs
2:
    /* Past the vector and the error code, which the processor will not do
     * for us: `iretq` expects to find RIP where they are sitting. */
    addq    $16, %rsp
    iretq
