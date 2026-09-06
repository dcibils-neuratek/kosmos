/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef ARCH_X86_64_USER_H
#define ARCH_X86_64_USER_H

#include <stdint.h>

/*
 * The user boundary: how a thread gets to ring 3, and how it gets back.
 *
 * `arch/aarch64/el0.S` is the counterpart of the first half and there is no
 * counterpart to the second, because ARM has none to have. An `svc` is an
 * exception like any other and lands in the vector table the machine
 * already had; `syscall` is a separate mechanism with its own entry
 * address, its own flag mask and its own idea of what it saves.
 */

/* Arms `syscall`: EFER.SCE, and the three MSRs that say where it goes, what
 * selectors it loads and which flags it clears. Needs gdt_init first. */
void user_init(void);

/*
 * Drops this thread to ring 3. Does not return: the thread is a process
 * from here on, and every entry into the kernel arrives as a `syscall` or
 * an exception.
 *
 * `arg` is the only thing a process is told, and it arrives in rdi.
 * Everything else it can reach is in its capability table, which is the
 * point: what a process has is what it was handed, and one word of
 * configuration does not change that.
 */
void enter_ring3(uintptr_t entry, uintptr_t user_sp, unsigned long arg);

/* What `syscall` ends up calling. Provided above `arch/`. */
uint64_t x86_syscall(uint64_t op, uint64_t arg);

#endif /* ARCH_X86_64_USER_H */
