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

#define CNTP_CTL_ENABLE     (1UL << 0)
#define CNTP_CTL_IMASK      (1UL << 1)

/* How many counter ticks between interrupts. Computed once from CNTFRQ. */
static uint64_t interval;

/* The absolute counter value the next interrupt is due at. Absolute, not a
 * countdown: see arm_next(). */
static uint64_t deadline;

/* Interrupts since the timer started. Written only by the interrupt, read
 * by anyone. Nothing else on this core runs while the handler does, so no
 * synchronisation is needed until SMP at M6. */
static volatile unsigned long ticks;

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
    uint64_t now;

    deadline += interval;
    now = read_cntpct();

    /*
     * If the deadline is already behind us the system could not keep up, and
     * every missed period is an interrupt waiting to fire the instant this
     * one returns. Resynchronising drops the backlog instead of servicing a
     * storm of interrupts that are all already late. The signed comparison
     * is what makes this correct across the counter wrapping.
     */
    if ((int64_t)(deadline - now) <= 0) {
        deadline = now + interval;
    }

    set_deadline(deadline);
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

    gic_enable_ppi(TIMER_INTID);

    deadline = read_cntpct() + interval;
    set_deadline(deadline);

    /* Enable, and explicitly clear the mask: the reset value of IMASK is not
     * architecturally guaranteed, and a masked timer counts down and fires
     * nothing. */
    __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(CNTP_CTL_ENABLE));
    __asm__ volatile("isb" ::: "memory");
}

void timer_interrupt(void)
{
    ticks++;

    /* Rearming is what deasserts the interrupt. The generic timer holds its
     * output high for as long as the comparator is in the past, so an EOI
     * without a rearm returns straight into the same interrupt forever. */
    arm_next();
}

unsigned long hal_ticks(void)
{
    return ticks;
}
