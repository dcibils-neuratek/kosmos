/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The 8253/8254 timer, at whatever rate the kernel asks for.
 *
 * `hal/qemu-virt/timer.c` programs the ARM generic timer, which counts at a
 * frequency the hardware reports and must be re-armed after every tick.
 * This one divides a fixed 1.193182 MHz - a number that is one third of the
 * original PC's 3.579545 MHz colour subcarrier, because in 1981 one crystal
 * was cheaper than two, and every PC since has kept it.
 *
 * Intel 8254 datasheet. The divisor is 16 bits, so the slowest tick this
 * can produce is about 18.2 Hz - which is where the DOS clock rate that
 * everyone remembers came from.
 */

#include <stdint.h>

#include "hal.h"
#include "pc.h"

#define PIT_CHANNEL0   0x40
#define PIT_COMMAND    0x43

#define PIT_HZ         1193182u

/* channel 0, low byte then high, mode 3 (square wave), binary */
#define PIT_SETUP      0x36

static volatile unsigned long ticks;

void hal_timer_init(unsigned hz)
{
    uint32_t divisor;

    if (hz == 0) {
        hz = 100;
    }

    divisor = PIT_HZ / hz;

    /*
     * A divisor of 0 means 65536 to the chip, which is the *slowest* rate
     * rather than the fastest - so the clamp at the top is the real one and
     * the one at the bottom would be an error made silent. A rate this
     * cannot express should be visible.
     */
    if (divisor > 0xFFFF) {
        divisor = 0xFFFF;
    }

    if (divisor == 0) {
        divisor = 1;
    }

    pc_out8(PIT_COMMAND, PIT_SETUP);
    pc_out8(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    pc_out8(PIT_CHANNEL0, (uint8_t)(divisor >> 8));

    pc_irq_unmask(0);
}

unsigned long hal_ticks(void)
{
    return ticks;
}

/*
 * How many deadlines came and went, and on this board the honest answer is
 * that it cannot be counted.
 *
 * On ARM the timer is one-shot: the handler computes the next deadline from
 * the last one and can see directly that it is already in the past, so a
 * missed period is a comparison it was making anyway. Mode 3 here free-runs
 * and re-fires on its own, so there is no re-arm to fall behind on - and
 * the 8259 collapses any number of repeats into one pending bit, so by the
 * time the handler runs the evidence is gone.
 *
 * It could be approximated by reading the IRR on every tick, and that would
 * put two port accesses in the interrupt path to undercount a diagnostic.
 * Returning 0 with the reason written down is better than a number that is
 * wrong in a direction nobody knows. The APIC has a proper answer and this
 * is one of the things that will pay for moving to it.
 */
unsigned long hal_ticks_missed(void)
{
    return 0;
}

/* Called from `pic.c` with interrupts off, which is what an interrupt gate
 * gets us: nothing else is touching `ticks` while this runs. */
void pc_timer_interrupt(void)
{
    ticks++;
}
