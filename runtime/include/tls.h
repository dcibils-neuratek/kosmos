/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_TLS_H
#define KOSMOS_TLS_H

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

#endif /* KOSMOS_TLS_H */
