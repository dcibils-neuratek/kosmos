/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The six syscall stubs as functions, for C that TinyCC built
 * (`syscall-aarch64.h`): its AArch64 assembler has no `svc`, so under it
 * `sysN` names these, which GCC compiled from the inline stubs. Every image
 * carries them; nothing GCC built calls them.
 */

#include "kosmos.h"

#if defined(__aarch64__)

long kosmos_sys0(long n);
long kosmos_sys1(long n, long a);
long kosmos_sys2(long n, long a, long b);
long kosmos_sys3(long n, long a, long b, long c);
long kosmos_sys4(long n, long a, long b, long c, long d);
long kosmos_sys5(long n, long a, long b, long c, long d, long e);

long kosmos_sys0(long n) { return sys0(n); }
long kosmos_sys1(long n, long a) { return sys1(n, a); }
long kosmos_sys2(long n, long a, long b) { return sys2(n, a, b); }
long kosmos_sys3(long n, long a, long b, long c) { return sys3(n, a, b, c); }
long kosmos_sys4(long n, long a, long b, long c, long d) { return sys4(n, a, b, c, d); }
long kosmos_sys5(long n, long a, long b, long c, long d, long e)
{
    return sys5(n, a, b, c, d, e);
}

#endif
