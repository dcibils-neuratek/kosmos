/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef ARCH_X86_64_GDT_H
#define ARCH_X86_64_GDT_H

/*
 * The descriptor tables, and the selectors everything else refers to.
 *
 * **AArch64 has no counterpart to this file at all.** A privilege level
 * there is a number in PSTATE, and dropping to EL0 is an `eret` with the
 * right bits in SPSR. On x86 a privilege level is a property of a *segment
 * descriptor*, so running at ring 3 means having a table of them, in the
 * one order the `syscall`/`sysret` pair will accept, plus a task state
 * segment holding the stack an entry into the kernel lands on.
 *
 * Segmentation itself is gone in long mode - bases and limits are ignored -
 * and what is left of it is exactly this: which ring, and 64-bit or not.
 *
 * **The order is not a convention, it is arithmetic.** `syscall` loads CS
 * from IA32_STAR[47:32] and SS from that plus 8. `sysretq` loads SS from
 * IA32_STAR[63:48] plus 8 and CS from that plus 16. So the kernel pair must
 * be adjacent in that order, the user pair must be adjacent in the
 * *opposite* order, and there is a slot between them that only a 32-bit
 * `sysretl` would ever load. Every x86-64 kernel has this layout, and they
 * all have it for this reason rather than by imitation.
 */

#define SEL_NULL        0x00
#define SEL_KERNEL_CODE 0x08
#define SEL_KERNEL_DATA 0x10
#define SEL_SYSRET_BASE 0x18    /* the slot the sysret arithmetic skips */
#define SEL_USER_DATA   0x20
#define SEL_USER_CODE   0x28
#define SEL_TSS         0x30    /* and 0x38: a system descriptor is 16 bytes */

/* Selectors as ring 3 sees them, with the requested privilege level in the
 * low two bits. What `iretq` and `sysretq` actually load. */
#define SEL_USER_DATA3  (SEL_USER_DATA | 3)
#define SEL_USER_CODE3  (SEL_USER_CODE | 3)

/* Where rsp0 sits in the TSS. `user.S` reads it directly, because on entry
 * from ring 3 through `syscall` there is no stack yet to find it any other
 * way. A _Static_assert in gdt.c keeps this honest. */
#define TSS_RSP0        4

#ifndef __ASSEMBLER__

#include <stdint.h>

/* Builds the tables, loads them, and reloads every segment register. */
void gdt_init(void);

/*
 * The stack an entry into the kernel lands on, for the thread about to run.
 *
 * This is what AArch64 gets for free from SP_EL1: the hardware selects a
 * different stack pointer on an exception from EL0, and `context_switch`
 * already carries it because it belongs to the thread. x86 keeps it in the
 * TSS instead, so it is not switched with the thread - it is *written* when
 * the thread is scheduled, which is one more thing the scheduler has to
 * remember to do and one more thing that is silently wrong if it forgets.
 */
void gdt_set_kernel_stack(uintptr_t top);

#endif /* !__ASSEMBLER__ */

#endif /* ARCH_X86_64_GDT_H */
