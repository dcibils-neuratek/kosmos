/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A futex: a thread sleeps while a word in its own process says what it
 * saw, and is woken by whoever changes it (`docs/w2.md` part 1, `futex.c`).
 */
#ifndef KOSMOS_FUTEX_H
#define KOSMOS_FUTEX_H

#include <stdbool.h>
#include <stdint.h>

struct process;
struct thread;

/* SYS_FUTEX_WAIT: 0 when woken, SYS_NO_CHANGE when the word was not
 * `expected`, SYS_TIMED_OUT when `wait_ticks` (0: none) ran out, and
 * SYS_ERR_FAULT for an address not four-byte aligned and writable. */
long futex_wait(struct process *p, uintptr_t va, uint32_t expected,
                unsigned long wait_ticks);

/* SYS_FUTEX_WAKE: up to `count` waiters on `va`, oldest first; how many. */
long futex_wake(struct process *p, uintptr_t va, unsigned long count);

/* A waiter's deadline came, or it is being ended: off its bucket, told it
 * timed out, and true - the caller wakes it. False, and nothing done, for
 * a thread not waiting on a futex, or one a waker took first. Called
 * wherever `ipc_timed_out` is, and by the kill path. */
bool futex_timed_out(struct thread *t);

/* Threads listed on any bucket. For tests. */
unsigned futex_listed(void);

#endif
