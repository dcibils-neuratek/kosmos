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

#endif /* KOSMOS_PROFILE_H */
