/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_SYSCALL_X86_64_H
#define KOSMOS_SYSCALL_X86_64_H

/*
 * The six syscall stubs, on x86-64.
 *
 * The number goes in rax and the arguments in rdi, rsi, rdx, r10, r8 - and
 * **r10 rather than rcx is the whole of what makes this different from a C
 * call.** System V passes the fourth argument in rcx, but `syscall` puts
 * the return address there before the kernel sees anything, so a fourth
 * argument passed the ABI's way is destroyed by the instruction that
 * delivers it. Linux uses r10 here for exactly this reason and this does
 * too; there is no other choice that works.
 *
 * The result comes back in rax, which is also where the number went. rcx
 * and r11 are clobbered by the instruction itself - the return address and
 * the flags - so both are named, and a compiler that had something live in
 * either spills it.
 *
 * `"D"`, `"S"` and `"d"` are rdi, rsi and rdx. r10 and r8 have no
 * constraint letter, so they are named with register variables, which is
 * the same thing said longhand.
 */

static inline long sys0(long n)
{
    long ret;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n)
                     : "rcx", "r11", "memory", "cc");
    return ret;
}

static inline long sys1(long n, long a)
{
    long ret;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a)
                     : "rcx", "r11", "memory", "cc");
    return ret;
}

static inline long sys2(long n, long a, long b)
{
    long ret;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b)
                     : "rcx", "r11", "memory", "cc");
    return ret;
}

static inline long sys3(long n, long a, long b, long c)
{
    long ret;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory", "cc");
    return ret;
}

static inline long sys4(long n, long a, long b, long c, long d)
{
    long ret;
    register long r10 __asm__("r10") = d;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory", "cc");
    return ret;
}

static inline long sys5(long n, long a, long b, long c, long d, long e)
{
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory", "cc");
    return ret;
}

#endif /* KOSMOS_SYSCALL_X86_64_H */
