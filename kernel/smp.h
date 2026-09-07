#ifndef KERNEL_SMP_H
#define KERNEL_SMP_H

/*
 * Bringing up the other processors.
 *
 * `kernel/smp.c` holds the whole of it and `docs/smp.md` holds the plan.
 * What exists is step three: the cores start, claim their own `struct
 * percpu`, and park. Nothing schedules on them yet.
 */

/* How many bytes of stack a secondary gets, of each of its two.
 *
 * Also used by `boot/start.S`, which is why it is a plain number a
 * preprocessor and an assembler can both read. Core 0 gets 16 KB from the
 * linker script; a core that only parks in `wfi` needs a fraction of that,
 * and 4 KB is a page. */
#define SECONDARY_STACK_BYTES   4096

#ifndef __ASSEMBLER__

/* Starts every processor the machine has that this kernel has a slot for.
 * Called once, from `kmain`, after the MMU and the per-CPU state exist.
 * Bounded: a core that never appears is reported, not waited for. */
void smp_start_others(void);

/* How many processors have run kernel code, counting this one. Not the
 * number that are scheduling threads, which is `thread_cpu_count`. */
unsigned smp_online(void);

/* Where a secondary lands. Called from `boot/start.S` and nowhere else. */
void secondary_main(unsigned long index);

#endif /* !__ASSEMBLER__ */

#endif /* KERNEL_SMP_H */
