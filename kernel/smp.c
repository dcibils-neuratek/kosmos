/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Starting the other processors, and stopping there.
 *
 * **`docs/smp.md` step three: a second core, doing nothing.** It is brought
 * up, it turns translation on with the tables core 0 built, it claims its
 * own `struct percpu`, it says so, and it parks. It touches no shared
 * structure, takes no lock, and runs no thread - which is exactly why it
 * can be done before there are any locks to take.
 *
 * That sounds like very little and it proves the four things that are
 * genuinely hard to be sure of otherwise:
 *
 *   - the firmware call works and the entry address was right;
 *   - a processor started this way can turn its own MMU on with tables it
 *     did not build;
 *   - `TPIDR_EL1` really is per-core - step one could only assert that
 *     from a machine with one core, where every answer is the same answer;
 *   - and the machine survives it. A second core running *anything* is the
 *     first time this kernel has had two instruction streams in it.
 *
 * **Nothing here is on the path of a running system.** `smp_start_others`
 * is called once at boot and never again; after it, the secondaries are in
 * `wfi` for ever and the machine behaves exactly as it did on one core.
 * That is the property `make test` checks by still passing.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "cpu.h"
#include "hal.h"
#include "kernel.h"
#include "percpu.h"
#include "smp.h"

/*
 * A stack per secondary, and two of them each.
 *
 * The same pair core 0 has and for the same reason `boot/kosmos.ld` gives
 * it two: this kernel runs on SP_EL0, so an exception switches to SP_EL1
 * and the handler stands on a stack that is not the one that faulted.
 *
 * In `.bss` rather than the linker script because they are indexed - the
 * entry code computes its own from the context word, and an array is the
 * shape that allows it. Core 0's stay in the linker script, where they can
 * have a guard page beneath them; these do not, and that is a real
 * difference worth naming rather than a detail: a secondary that overflows
 * walks into the slot below it instead of faulting. It parks in `wfi`, so
 * it never comes close, and the day one runs threads it wants the linker
 * script treatment too.
 *
 * `_Alignas(16)` because AArch64 requires a 16-byte-aligned stack pointer
 * and the entry code does no rounding.
 */
_Alignas(16) uint8_t secondary_stacks[NR_CPUS][SECONDARY_STACK_BYTES];
_Alignas(16) uint8_t secondary_exception_stacks[NR_CPUS][SECONDARY_STACK_BYTES];

/*
 * How many processors have run kernel code.
 *
 * One before any of this - core 0 counts itself. **Not the same number as
 * how many are scheduling**, which is `thread_cpu_count` and is still one:
 * a parked core is in the kernel and is not running threads, and collapsing
 * those two would be claiming step four before it is done.
 *
 * `volatile` because a secondary writes it and core 0 reads it in a loop
 * below, which is the first time in this kernel that a variable is touched
 * by two instruction streams. It is not a lock and does not need to be:
 * one writer per slot, and the reader only ever waits for it to rise.
 */
static volatile unsigned online = 1;

unsigned smp_online(void)
{
    return online;
}

/*
 * Where a secondary lands, in C, with a stack and translation on.
 *
 * **Everything before this is architecture**, and deliberately so - this
 * file has none in it. `boot/start.S` gives the core a known SCTLR, two
 * stacks and its page tables, and calls this; how much of that a machine
 * needs is exactly what differs between the two of them.
 */
void secondary_main(unsigned long index)
{
    /*
     * The first thing it does is claim its own `struct percpu`, found
     * through its own per-CPU register - which is the whole of what step
     * one built and could not demonstrate on a machine with one processor.
     */
    percpu_init((unsigned)index);

    /*
     * The slot before the count, and the barrier is what makes it so.
     *
     * `online` is what core 0 waits on, and `arch/<name>/cpu.h` says why a
     * `volatile` on it was never enough: it stops the compiler caching the
     * variable and says nothing about the order two stores become visible
     * in. Without this a weakly ordered machine may show core 0 the count
     * rising before the slot is written, which is a suite that passes every
     * time until it does not.
     */
    cpu_publish();
    online++;

    /*
     * And that is all it does.
     *
     * `wfi` rather than a spin, so a parked core costs nothing and QEMU can
     * tell it is idle. It wakes on any interrupt - none is routed here -
     * and goes straight back, which is the correct shape for "this core has
     * nothing to do" and is what the idle thread will do when step four
     * gives it one.
     */
    for (;;) {
        cpu_wait_for_interrupt();
    }
}

void smp_start_others(void)
{
    uintptr_t entry   = cpu_secondary_entry();
    unsigned  present = hal_cpu_count();
    unsigned  want    = (present < NR_CPUS) ? present : NR_CPUS;
    unsigned  i;

    /*
     * An architecture with nowhere to land a core does not get asked. x86-64
     * is that architecture today and says why in `arch/x86_64/cpu.h`; the
     * board there refuses as well, and the two refusals are separate on
     * purpose.
     */
    if (entry == 0) {
        return;
    }

    for (i = 1; i < want; i++) {
        unsigned long spins;
        unsigned      was = online;

        if (!hal_cpu_on(i, entry, i)) {
            continue;               /* a board that cannot, or a refusal */
        }

        /*
         * Wait for it to say it arrived, and give up rather than hang.
         *
         * **A bounded wait, because the alternative is a machine that boots
         * on one core and stops.** A processor the firmware accepted and
         * that never appears is a bug worth reporting from a running
         * system, not one worth hanging in front of - and this is the first
         * code here that waits on another instruction stream at all.
         *
         * The bound is spins rather than ticks because interrupts are still
         * masked at this point in the boot: `hal_ticks` would not advance.
         */
        for (spins = 0; spins < 100000000UL && online == was; spins++) {
            cpu_relax();
        }

        /* The other half of the pair. Everything the secondary wrote before
         * its `cpu_publish` is visible to this core from here on. */
        cpu_observe();
    }
}
