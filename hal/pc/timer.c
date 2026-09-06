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

#include "cpu.h"
#include "hal.h"
#include "pc.h"

#define PIT_CHANNEL0   0x40
#define PIT_COMMAND    0x43

#define PIT_HZ         1193182u

/* channel 0, low byte then high, mode 3 (square wave), binary */
#define PIT_SETUP      0x36

static volatile unsigned long ticks;

/*
 * How fast the cycle counter runs, measured against the one clock on this
 * board whose frequency is a known constant.
 *
 * The PIT has three channels and channel 2 is the one nothing else uses -
 * it was wired to the PC speaker, and its gate and output are two bits of
 * port 0x61 that software can drive and watch. So: arm it for a known
 * interval, read the TSC, wait for its output to go high, read the TSC
 * again. The ratio is the answer.
 *
 * Ten milliseconds, which is long enough that the cost of the two reads is
 * noise and short enough not to be felt in a boot. Mode 0 - interrupt on
 * terminal count - because what is wanted is one edge at a known time
 * rather than a wave.
 *
 * The speaker bit is explicitly cleared. Bit 1 of port 0x61 connects
 * channel 2's output to a physical speaker, and calibrating the clock is
 * not a reason to make a noise.
 */
#define PIT_CHANNEL2    0x42
#define PIT_GATE2       0x61

#define GATE2_ON        (1u << 0)
#define SPEAKER_ON      (1u << 1)
#define OUT2_HIGH       (1u << 5)

#define CALIBRATE_MS    10u

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;

    /* `lfence` first, for the reason `isb` is there on ARM: without it the
     * read can be reordered ahead of what it is timing. */
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));

    return ((uint64_t)hi << 32) | lo;
}

static uint64_t calibrate(void)
{
    uint32_t divisor = (PIT_HZ * CALIBRATE_MS) / 1000u;
    uint64_t start, end;
    uint8_t gate;
    unsigned spins;

    /* Channel 2, low byte then high, mode 0, binary. */
    pc_out8(PIT_COMMAND, 0xB0);
    pc_out8(PIT_CHANNEL2, (uint8_t)(divisor & 0xFF));
    pc_out8(PIT_CHANNEL2, (uint8_t)(divisor >> 8));

    /* The gate low then high is what starts it counting; the speaker stays
     * disconnected throughout. */
    gate = (uint8_t)(pc_in8(PIT_GATE2) & ~(GATE2_ON | SPEAKER_ON));
    pc_out8(PIT_GATE2, gate);
    pc_out8(PIT_GATE2, (uint8_t)(gate | GATE2_ON));

    start = rdtsc();

    /*
     * Bounded, because this is the boot path: a channel that never reaches
     * terminal count must be a machine that starts with an unknown counter
     * rather than one that never starts. Ten million spins is far longer
     * than ten milliseconds on anything, real or emulated.
     */
    for (spins = 0; spins < 10000000u; spins++) {
        if ((pc_in8(PIT_GATE2) & OUT2_HIGH) != 0) {
            break;
        }
    }

    end = rdtsc();

    pc_out8(PIT_GATE2, gate);   /* gate off again */

    if (spins >= 10000000u || end <= start) {
        return 0;
    }

    return ((end - start) * 1000ULL) / CALIBRATE_MS;
}

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

    /*
     * And what the cycle counter turned out to be running at, which the
     * processor could not say. `arch/x86_64/cpu.h` explains why the board
     * is the one that can answer and why the answer matters: it reaches
     * userland as `/dev/cpu`'s `counter_hz`, and a zero there is a
     * division nobody guarded against.
     */
    cpu_set_counter_hz(calibrate());
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

const char *hal_timer_describe(void)
{
    return "the 8253 through a pair of 8259s";
}
