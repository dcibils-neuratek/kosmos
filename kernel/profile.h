/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_PROFILE_H
#define KOSMOS_PROFILE_H

/*
 * Where every processor is, a tick at a time (`SYS_PROFILE`, `profile.c`).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct process;

/*
 * From each board's tick, with the address the processor was interrupted at
 * and whether a program was running there. One comparison when no profile
 * runs.
 */
void profile_tick(uint64_t pc, bool user);

long profile_call(struct process *p, unsigned long op, uintptr_t buf, size_t max);

/*
 * Around every syscall: whether to time it, and what it cost. A syscall
 * runs with interrupts masked and a thread never leaves its processor, so
 * each processor's counts are its own to add to.
 */
bool profile_counting(void);
void profile_syscall(unsigned long number, uint64_t counter_ticks);

#endif /* KOSMOS_PROFILE_H */
