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
 * `enter_user` is declared in `context.h`, for both architectures. It is
 * implemented in `user.S` here and in `el0.S` there, and dropping to ring 3
 * is what it means on this machine.
 */

/* What `syscall` ends up calling. Provided above `arch/`. */
uint64_t x86_syscall(uint64_t op, uint64_t arg);

#endif /* ARCH_X86_64_USER_H */
