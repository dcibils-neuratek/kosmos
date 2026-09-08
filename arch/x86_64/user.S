/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
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
 * **`user_rsp` is live for exactly two instructions, and it took a bug to
 * get it there.** It was a global the whole way through the syscall - park
 * the process's stack pointer on the way in, restore it on the way out -
 * and the note beside it said it was per-CPU state in everything but name,
 * needing `swapgs` only when a second core arrived.
 *
 * That was wrong, and one core was enough to prove it. The state is
 * per-*thread*: a process that blocks inside a syscall - which is every
 * IPC - lets another process run, that process makes its own syscall and
 * writes the global, and the first one returns to ring 3 on somebody
 * else's stack. What it looked like was `l_call` returning with rsp four
 * kilobytes *above* the top of the stack, in a process that had just
 * printed its capabilities correctly.
 *
 * AArch64 never asks the question: an exception writes its frame onto the
 * thread's own SP_EL1 stack, so the saved SP_EL0 is per-thread by
 * construction. Here the fix is to get onto the kernel stack and push it
 * there in the same breath - which is what the three instructions below
 * do, with the global as a scratch cell across two of them, at a moment
 * IA32_FMASK has already made uninterruptible.
 */
.global syscall_entry
syscall_entry:
    movq    %rsp, user_rsp(%rip)
    movq    tss+TSS_RSP0(%rip), %rsp
    pushq   user_rsp(%rip)          /* and now it belongs to this thread */

    /*
     * **Everything the process had, because `syscall` saved none of it.**
     *
     * This is the difference from an exception that costs the most lines.
     * AArch64's `svc` goes through the vector table like any other
     * exception: the entry stub writes all thirty-one general registers
     * into the trapframe and `eret` reads them back, so `kernel/syscall.c`
     * can do what it likes and a process notices nothing. `syscall` writes
     * two registers - rcx and r11 - and leaves the other fifteen exactly as
     * the process had them, which means the kernel is now running with a
     * process's values in registers the C it is about to call will clobber.
     *
     * The convention is that a syscall preserves everything but rax, rcx
     * and r11. Getting it wrong is not a crash: it is a caller whose live
     * variable changed under it. What it looked like here was
     *
     *     lua_pushinteger(L, sys0(SYS_ENDPOINT));
     *
     * where GCC quite correctly kept `L` in rdi across the `syscall` - and
     * this stub had put the frame pointer there.
     *
     * rcx and r11 first because `sysretq` needs them and the C call is free
     * to clobber both. The rest because a process is entitled to them.
     * Eight pushes is sixty-four bytes, which leaves rsp sixteen-aligned.
     */
    pushq   %rcx                    /* the return address the CPU saved */
    pushq   %r11                    /* and the flags */
    pushq   %r9
    pushq   %r8
    pushq   %r10
    pushq   %rdx
    pushq   %rsi
    pushq   %rdi

    /*
     * `struct syscall_frame`, built on the stack in the layout
     * `kernel/syscall.h` declares: number, five arguments, then the result.
     * Fifty-six bytes rounded to seventy-two, because the `call` below
     * wants rsp+8 aligned to sixteen at the callee's entry: the nine pushes
     * above leave rsp eight out, so the frame carries the odd eight rather
     * than a padding push that would have to be explained twice.
     *
     * **r10 rather than rcx for argument three**, which is the one place
     * this differs from a C call and is not a choice: `syscall` puts the
     * return address in rcx before the kernel sees anything, so an argument
     * passed the ABI's way would be destroyed by the instruction delivering
     * it. `user/include/syscall-x86_64.h` puts it in r10 for this reason
     * and Linux does the same.
     */
    subq    $72, %rsp

    movq    %rax,  0(%rsp)          /* number */
    movq    %rdi,  8(%rsp)          /* arg 0 */
    movq    %rsi, 16(%rsp)          /* arg 1 */
    movq    %rdx, 24(%rsp)          /* arg 2 */
    movq    %r10, 32(%rsp)          /* arg 3 */
    movq    %r8,  40(%rsp)          /* arg 4 */
    movq    $0,   48(%rsp)          /* result */

    movq    %rsp, %rdi
    call    syscall_dispatch

    movq    48(%rsp), %rax
    addq    $72, %rsp

    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %r10
    popq    %r8
    popq    %r9
    popq    %r11
    popq    %rcx

    /* And the process's own stack, off this thread's kernel stack rather
     * than out of a global that another thread may have written. */
    popq    %rsp

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
