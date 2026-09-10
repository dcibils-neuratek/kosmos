/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The first C that runs on x86-64, and the last thing here that is not the
 * kernel proper.
 *
 * `boot/x86_64/start.S` cannot call `kmain` directly because the processor
 * is not yet in a state `kmain` may assume: there is no GDT this kernel
 * built, no TSS, no `syscall` instruction armed, and SSE raises #UD until
 * an operating system claims it.
 *
 * Three calls, and `boot/start.S` on ARM ends with `bl kmain` because
 * ARM's equivalent of all three is nothing at all - a core comes up able to
 * execute floating point, its exception level is a number in PSTATE rather
 * than a segment descriptor, and `svc` needs no arming.
 *
 * What is deliberately *not* here is anything about multiboot. That is the
 * board's handoff, `hal/pc/` keeps it, and `arch/` has no more business
 * knowing about it than the ARM side has knowing about a device tree.
 */

#include <stdint.h>

#include "gdt.h"
#include "mmu.h"
#include "smp.h"
#include "user.h"

void kmain(void);
void fp_init(void);

void kmain_x86(void)
{
    /*
     * The descriptor tables, and this must happen before `mmu_init` narrows
     * .rodata to read-only.
     *
     * The processor *writes* the accessed bit into a descriptor when a
     * segment register is loaded. `start.S`'s table is in .rodata, so a
     * descriptor with that bit still clear would fault on the next
     * interrupt - from inside the interrupt. The table `gdt_init` builds is
     * in .bss and writable, which removes the question rather than relying
     * on the first tick having already set the bit.
     */
    gdt_init();
    user_init();

    /* And SSE, which x86 requires an operating system to claim before it
     * will execute a floating-point instruction. `fp.c` says why. */
    fp_init();

    kmain();
}

/*
 * Where a started processor reaches C: the calls core zero made above, for
 * this core, and then the kernel's own secondary path.
 *
 * **The order is load-bearing.** The GDT and this core's TSS first, so that
 * anything taken from here lands on this core's stacks. Then the kernel's
 * page tables, off the boot tables the climb used. Then `syscall`'s MSRs and
 * floating point, both per core and both only ever set on core zero. Then
 * `secondary_main`, whose first act is claiming this core's per-CPU block
 * through GS.
 *
 * **And each step writes how far it got into `reached`**, the word the board
 * handed over and reads back - `hal/pc/trampoline.h` names the numbers - so a
 * processor that stops between here and `secondary_main` says where, rather
 * than only that it never arrived. The word is in low memory, which the
 * kernel's tables map, so it stays writable across `mmu_enable_here`.
 */
void x86_secondary_entry(unsigned long index, uint32_t *reached)
{
    *reached = 5;

    gdt_init_here((unsigned)index);
    *reached = 6;

    mmu_enable_here();
    *reached = 7;

    user_init();
    fp_init();
    *reached = 8;

    secondary_main(index);
}
