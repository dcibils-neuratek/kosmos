/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_SYS_RESOURCE_H
#define POSIX_SYS_RESOURCE_H

/*
 * **What a process has used**, for ported code (`docs/w4.md` W4c): WTF's
 * CPU time and its allocator's statistics ask `getrusage`. Of the process
 * itself only (`RUSAGE_SELF`); the times and the memory are not measured
 * yet and read as nought - Nebula counts a thread's ticks and hands no
 * process its own (`roadmap.md`, W4c's follow-ups) - so a caller sees
 * nothing used rather than a number that is wrong.
 */

#include <stddef.h>
#include <sys/cdefs.h>
#include <sys/time.h>

__BEGIN_DECLS

#define RUSAGE_SELF 0

struct rusage {
    struct timeval ru_utime;    /* running its own code */
    struct timeval ru_stime;    /* the kernel, for it */
    long           ru_maxrss;   /* the most memory it held, in KB */
};

int getrusage(int who, struct rusage *out);

/* Kosmos's: the memory this process holds now, in bytes, from Nebula's
 * process table (`process.c`) - what Linux's ports read from /proc. */
size_t posix_footprint_np(void);

__END_DECLS

#endif /* POSIX_SYS_RESOURCE_H */
