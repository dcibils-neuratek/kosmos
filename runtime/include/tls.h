/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_TLS_H
#define KOSMOS_TLS_H

#include <sys/cdefs.h>
__BEGIN_DECLS

/*
 * A thread's own block: what the thread pointer points at (`threads.md`
 * step 2).
 *
 * **`self` is first and is the block's own address**, because the two boards
 * answer "where is my block" differently. AArch64 hands back the register
 * itself (`TPIDR_EL0`); x86 cannot read the FS base from user mode at all,
 * and what a process can do is read *through* it - `%fs:0` - so the first
 * word has to be the pointer. One layout, one way to ask, and the difference
 * stays inside `kosmos_tls()`.
 *
 * `errno` is what lives here today. A thread's identity and whatever a
 * library wants to keep per thread come with the threads themselves.
 */
struct tls_block {
    void *self;
    int   errno_value;
};

/*
 * **The kernel makes the block**, one page at the bottom of a thread's stack
 * slot, before the thread runs (`USER_TBLOCK` in `kernel/process.h`), so
 * `errno` works from a thread's first instruction. A program that wants a
 * larger one of its own points the register at it with `kosmos_set_tls`.
 */

/*
 * **A thread's own variables** (`runtime/libc/tls.c`): `kosmos_tls_begin`
 * makes this thread's copy of the image's `thread_local` template and points
 * the thread pointer at it - NULL, and nothing changed, when the image has
 * none - and `kosmos_tls_end` gives it back. `main` begins one for the first
 * thread; a thread started with `kosmos_thread_start` that uses such
 * variables begins its own as its first act, **in a function that uses none
 * of them itself**: the compiler reads the thread pointer once a function,
 * so the variables are used from a function called after the begin.
 */
#include <stddef.h>
size_t kosmos_tls_bytes(void);
void  *kosmos_tls_begin(void);
void   kosmos_tls_end(void *made);
void   kosmos_run_constructors(void);

__END_DECLS

#endif /* KOSMOS_TLS_H */
