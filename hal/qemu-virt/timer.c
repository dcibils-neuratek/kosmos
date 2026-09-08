/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The ARM generic timer.
 *
 * Almost none of this is board specific: CNTFRQ_EL0, CNTP_TVAL_EL0 and
 * CNTP_CTL_EL0 are architectural and identical on every ARMv8-A part. What
 * belongs to the board is which interrupt the timer is wired to, which is why
 * this lives in hal/ for now.
 *
 * When the second target lands at M2 this file will be a near copy of the
 * Pi's, and that is the signal to split it: the system registers move to
 * arch/aarch64/ and only the INTID stays here. Doing that split now, with a
 * single target, would be inventing the boundary instead of finding it.
 *
 * The EL1 physical timer is used rather than the virtual one: the kernel
 * runs at EL1 with nothing above it, so CNTP is the one that belongs to us.
 */

#include <stdint.h>

#include "hal.h"
#include "qemu-virt.h"
#include "panic.h"
#include "percpu.h"

#define CNTP_CTL_ENABLE     (1UL << 0)
#define CNTP_CTL_IMASK      (1UL << 1)

/*
 * How many counter ticks between interrupts. Computed once from CNTFRQ.
 *
 * The one thing here that is the machine's rather than a processor's: every
 * core's generic timer counts the same system counter at the same rate, so
 * they all want the same interval. Written once, before any secondary
 * exists, and read-only afterwards.
 */
static uint64_t interval;

/*
 * And these three are per processor, because the timer is.
 *
 * **CNTP_CVAL_EL0 and CNTP_CTL_EL0 are banked by architecture**: each core
 * has its own comparator, arms it itself, and receives PPI 30 when its own
 * fires. There is no sense in which four cores share a deadline - and as
 * file statics these were four cores writing one `deadline` and one `ticks`
 * from four interrupt handlers, which is a corrupted comparator on the first
 * tick and a tick count that means nothing.
 *
 * No lock, and none needed: every entry is written only by the core it
 * belongs to, from that core's interrupt handler, with interrupts masked.
 * `hal_ticks` reads its own. Reading another core's is what
 * `hal_ticks_on` is for, and it is a torn-read-tolerant unsigned long that
 * only ever rises.
 */
static uint64_t deadline[NR_CPUS];
static volatile unsigned long ticks[NR_CPUS];
static volatile unsigned long missed[NR_CPUS];

/* Which processor this is, for indexing the three above. */
static inline unsigned here(void)
{
    return this_cpu()->index;
}

static inline uint64_t read_cntfrq(void)
{
    uint64_t hz;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));
    return hz;
}

static inline uint64_t read_cntpct(void)
{
    uint64_t now;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
    return now;
}

static inline void set_deadline(uint64_t at)
{
    __asm__ volatile("msr cntp_cval_el0, %0" : : "r"(at));
}

/*
 * Rearming uses CVAL, an absolute deadline, and each one is computed from
 * the previous deadline rather than from the current time.
 *
 * The countdown register, TVAL, is the obvious choice and it is wrong. It
 * sets the comparator to "now plus interval", where "now" is the moment the
 * handler runs, so every period silently absorbs the latency of taking the
 * interrupt. That error does not average out, it accumulates.
 *
 * It was measured rather than reasoned about. Under QEMU's TCG, taking an
 * interrupt costs about 195,000 counter ticks, roughly 3 ms, against a 10 ms
 * period. Rearming from "now" made a nominal 100 Hz tick run at 73 Hz: eight
 * ticks in eleven seconds of wall clock. Computing from the previous
 * deadline makes the period exact as long as the handler finishes inside it.
 */
static void arm_next(void)
{
    unsigned cpu = here();
    uint64_t now;

    deadline[cpu] += interval;
    now = read_cntpct();

    /*
     * If the deadline is already behind us the system could not keep up, and
     * every missed period is an interrupt waiting to fire the instant this
     * one returns. Resynchronising drops the backlog instead of servicing a
     * storm of interrupts that are all already late. The signed comparison
     * is what makes this correct across the counter wrapping.
     */
    if ((int64_t)(deadline[cpu] - now) <= 0) {
        /*
         * Counted rather than silently absorbed. Resynchronising is the
         * right response to falling behind, but it breaks the relationship
         * between the tick count and elapsed time, and something that
         * quietly stops being true is worse than something that says so.
         */
        missed[cpu]++;
        deadline[cpu] = now + interval;
    }

    set_deadline(deadline[cpu]);
}

void hal_timer_init(unsigned hz)
{
    uint64_t frequency = read_cntfrq();

    if (frequency == 0) {
        /* Firmware is supposed to program CNTFRQ_EL0 before handing over. If
         * it is zero there is no way to know how fast the counter runs, and
         * every interval computed from it would be wrong. */
        panic("timer: CNTFRQ_EL0 is zero");
    }

    interval = frequency / hz;

    /* And this core's own comparator. Core zero is a core like any other. */
    hal_timer_init_here();
}

/*
 * Arm *this* processor's timer, with the interval the machine already chose.
 *
 * Everything here is banked or private: the PPI is enabled in this core's
 * redistributor, the comparator is this core's CNTP_CVAL_EL0, and the enable
 * is this core's CNTP_CTL_EL0. A secondary calls it for itself as it comes
 * up, and until it does it receives no timer interrupt at all - which is
 * exactly why a parked core costs nothing.
 *
 * `interval` is read and never written here, so this needs no ordering
 * against `hal_timer_init`: it runs long after, on a core that was started
 * by one that had already computed it.
 */
void hal_timer_init_here(void)
{
    unsigned cpu = here();

    if (interval == 0) {
        panic("timer: armed on a processor before the machine set its rate");
    }

    gic_enable_ppi(TIMER_INTID);

    deadline[cpu] = read_cntpct() + interval;
    set_deadline(deadline[cpu]);

    /* Enable, and explicitly clear the mask: the reset value of IMASK is not
     * architecturally guaranteed, and a masked timer counts down and fires
     * nothing. */
    __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(CNTP_CTL_ENABLE));
    __asm__ volatile("isb" ::: "memory");
}

void timer_interrupt(void)
{
    ticks[here()]++;

    /* Rearming is what deasserts the interrupt. The generic timer holds its
     * output high for as long as the comparator is in the past, so an EOI
     * without a rearm returns straight into the same interrupt forever. */
    arm_next();
}

unsigned long hal_ticks(void)
{
    return ticks[here()];
}

/*
 * Another processor's tick count.
 *
 * The only way to tell a secondary that is alive and taking interrupts from
 * one that started, parked, and died - which nothing in this system could do
 * before, and which is what the test for `docs/smp.md` step four asserts.
 */
unsigned long hal_ticks_on(unsigned cpu)
{
    if (cpu >= NR_CPUS) {
        return 0;
    }

    return ticks[cpu];
}

unsigned long hal_ticks_missed(void)
{
    return missed[here()];
}

const char *hal_timer_describe(void)
{
    return "the generic timer through a GICv3";
}
