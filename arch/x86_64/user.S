/*
 * Getting to ring 3, and getting back.
 *
 * `arch/aarch64/el0.S` is the first half's counterpart, and the difference
 * is that a privilege level on ARM is bits in PSTATE while here it is which
 * segment descriptor CS names. So an `eret` there becomes an `iretq` here
 * with a five-word frame built by hand - and building it by hand is the
 * point: the same frame an interrupt would have pushed, so the return path
 * is the one the hardware already knows.
 *
 * There is no counterpart to the second half, because ARM has none to have.
 * An `svc` is an exception like any other and lands in the vector table the
 * machine already had. `syscall` is a separate mechanism: its own entry
 * address in an MSR, its own flag mask, and - the part that decides how
 * this file is written - **no stack switch at all**.
 */

#include "gdt.h"

.section .text
.global enter_user

/*
 * void enter_user(uintptr_t entry, uintptr_t user_sp, unsigned long arg);
 *   rdi = where ring 3 starts
 *   rsi = the process's own stack
 *   rdx = the one word it is told
 */
enter_user:
    /*
     * Nothing may interrupt the transition. Between the segment loads and
     * the iretq this is ring 0 code with ring 3 data selectors in ds and
     * es, and an interrupt taken there would run a handler with them.
     *
     * Nothing is lost by masking: the rflags word pushed below has IF set,
     * so the process starts with interrupts enabled. A process that could
     * not be preempted would keep the machine the moment it chose not to
     * make a syscall.
     */
    cli

    movq    %rdx, %r8               /* the argument, out of the way */

    movl    $(SEL_USER_DATA | 3), %eax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs

    /*
     * The frame `iretq` pops, in the order it pops it: rip, cs, rflags,
     * rsp, ss - so pushed bottom up. The RPL of 3 in each selector is not
     * decoration: it is what the processor compares to decide the new CPL,
     * and a selector pushed without it returns to ring 0 with a user
     * descriptor, which is either a fault or a disaster depending on which.
     */
    pushq   $(SEL_USER_DATA | 3)
    pushq   %rsi
    pushq   $0x202                  /* IF set; bit 1 reads as 1 always */
    pushq   $(SEL_USER_CODE | 3)
    pushq   %rdi

    /*
     * Nothing is left in a register on purpose. Whatever the kernel had in
     * them would be visible to ring 3, and a kernel pointer handed over by
     * accident defeats the address space it is about to be confined to.
     */
    movq    %r8, %rdi
    xorl    %eax, %eax
    xorl    %ebx, %ebx
    xorl    %ecx, %ecx
    xorl    %edx, %edx
    xorl    %esi, %esi
    xorl    %ebp, %ebp
    xorl    %r8d, %r8d
    xorl    %r9d, %r9d
    xorl    %r10d, %r10d
    xorl    %r11d, %r11d
    xorl    %r12d, %r12d
    xorl    %r13d, %r13d
    xorl    %r14d, %r14d
    xorl    %r15d, %r15d

    iretq

/*
 * Where `syscall` arrives.
 *
 * The processor has put the return address in rcx and the flags in r11,
 * cleared whatever IA32_FMASK names, and loaded CS and SS from IA32_STAR.
 * **It has not touched rsp**, so the first instruction here runs at ring 0
 * on a stack the process chose - which is why IA32_FMASK clears IF, and why
 * the two instructions that follow are the only ones that may run before
 * the stack is the kernel's.
 *
 * The kernel stack comes out of the TSS, which is where an interrupt from
 * ring 3 would have got it too. That keeps one answer to "which stack does
 * an entry land on" rather than two that can disagree.
 *
 * `user_rsp` is per-CPU state in everything but name, and so is the TSS.
 * `CLAUDE.md` forbids loose mutable globals for exactly this reason, and
 * `fp.c` has the same note for the same kind of variable: with one core
 * there is one of each, and when there is a second, both belong in a
 * per-CPU structure reached through the GS base - which is what `swapgs`
 * exists for and what this entry will grow.
 */
.global syscall_entry
syscall_entry:
    movq    %rsp, user_rsp(%rip)
    movq    tss+TSS_RSP0(%rip), %rsp

    /*
     * Both of these are the process's, not scratch: `sysretq` reads the
     * return address out of rcx and the flags out of r11, and the C call
     * below is free to clobber either.
     */
    pushq   %rcx
    pushq   %r11

    /* rax holds what to do and rdi what to do it to, which is the shape a
     * declared struct will take when there is a real protocol here. */
    movq    %rdi, %rsi
    movq    %rax, %rdi
    call    x86_syscall

    popq    %r11
    popq    %rcx
    movq    user_rsp(%rip), %rsp

    /*
     * Back to ring 3 with the answer in rax. `sysretq` rather than `sysret`
     * - without the REX.W prefix it returns to 32-bit compatibility mode,
     * which would be a process that suddenly cannot see its own pointers.
     */
    sysretq

.section .bss
.align 8
user_rsp:
    .skip 8
