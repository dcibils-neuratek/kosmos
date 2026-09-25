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

#include <stdbool.h>
#include <stdint.h>

#include "console.h"
#include "cpu.h"
#include "hal.h"
#include "apic.h"
#include "ec.h"
#include "pc.h"
#include "percpu.h"

#define PIT_CHANNEL0   0x40
#define PIT_COMMAND    0x43

#define PIT_HZ         1193182u

/* channel 0, low byte then high, mode 3 (square wave), binary */
#define PIT_SETUP      0x36

static volatile unsigned long ticks[NR_CPUS];

/* Which chip is producing them, for the boot log. */
static bool on_apic_timer;

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

/*
 * **A look at the chip says when the loop noticed, not when the count ran
 * out** - and on 25 September that difference was the whole answer. The
 * gate ran twenty-six suites at once on the Mac, QEMU's thread was set
 * aside across the moment channel two finished, the loop saw it tens of
 * milliseconds late, and the TSC came out at 4.8 times its speed: a film of
 * three seconds took 0.62 by its own clock. Nothing here was wrong for a
 * machine that is never interrupted, and no machine is that - a System
 * Management Interrupt does the same on silicon.
 *
 * So each end of the interval is bracketed rather than read (`look`), and a
 * measurement whose brackets are wider than 1/CALIBRATE_SLACK of it was
 * interrupted and is taken again: about 0.4%, forty microseconds in ten
 * milliseconds, and the midpoint is kept, so the answer is within half
 * that. The narrowest of CALIBRATE_TRIES is used if none is that good, and
 * the boot log says which it was.
 */
#define CALIBRATE_SLACK 256u
#define CALIBRATE_TRIES 16u

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;

    /* `lfence` first, for the reason `isb` is there on ARM: without it the
     * read can be reordered ahead of what it is timing. */
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));

    return ((uint64_t)hi << 32) | lo;
}

/*
 * A busy wait of some milliseconds, on the 8253's channel two.
 *
 * **The one clock that is running before any other is trusted.** Channel
 * two is the one wired to the speaker rather than to an interrupt line, so
 * it can be started and watched without disturbing anything - which is why
 * `calibrate` below measures the TSC against it, and the local APIC's timer
 * is measured against the TSC in turn.
 *
 * A wait is not a measurement: it ends when the loop notices, which can be
 * late but never early, and that is all a wait promises. `look` is what
 * measuring on this channel takes.
 *
 * The divisor is sixteen bits, so at 1.193 MHz this tops out around 54 ms.
 * Nothing asks for more; a caller that did would get a shorter wait than it
 * wanted, so the argument is clamped rather than silently wrapped.
 */
/*
 * **The TSC's rate, measured once against the 8253 while it still counts.**
 *
 * The ThinkPad stopped in its boot on 19 September, stick 0.10.87, straight
 * after the machine was switched into ACPI mode and before the sound card
 * said anything - and the sound card's setup is a run of waits on the
 * 8253's channel two, each bounded at ten million reads of its port, which
 * is a few milliseconds under QEMU and ten seconds a wait on silicon. The
 * likeliest reason the channel stopped reaching its count is Intel's 8254
 * clock gating, which a firmware may switch on once an operating system
 * says it has taken over; `pc_pit_counts` asks the machine, and the boot
 * log says what it answered (`ec.c`).
 *
 * So the TSC is measured against the 8253 before that switch
 * (`pc_timer_measure_tsc`), and every wait from then on is on the TSC -
 * which is invariant on every processor this runs on and needs no chip
 * outside the core. Before it is measured, the 8253 as before.
 */
static uint64_t tsc_hz;
static bool tsc_measured;

static uint64_t calibrate(void);

/* Once, whoever asks first: a second measurement would be against a chip
 * that ACPI mode may have stopped since, and a channel that never finished
 * costs ten seconds a look on silicon. */
void pc_timer_measure_tsc(void)
{
    if (!tsc_measured) {
        tsc_measured = true;
        tsc_hz = calibrate();
    }
}

void pc_timer_wait_ms(unsigned ms)
{
    uint32_t divisor;
    uint8_t gate;
    unsigned spins;

    if (ms == 0) {
        return;
    }

    if (ms > 50u) {
        ms = 50u;
    }

    if (tsc_hz != 0) {
        uint64_t until = rdtsc() + tsc_hz * ms / 1000u;

        while (rdtsc() < until) {
            __asm__ volatile("pause");
        }

        return;
    }

    divisor = (PIT_HZ * ms) / 1000u;

    pc_out8(PIT_COMMAND, 0xB0);         /* channel 2, mode 0, binary */
    pc_out8(PIT_CHANNEL2, (uint8_t)(divisor & 0xFF));
    pc_out8(PIT_CHANNEL2, (uint8_t)(divisor >> 8));

    gate = (uint8_t)(pc_in8(PIT_GATE2) & ~(GATE2_ON | SPEAKER_ON));
    pc_out8(PIT_GATE2, gate);
    pc_out8(PIT_GATE2, (uint8_t)(gate | GATE2_ON));

    /* Bounded for `calibrate`'s reason: a channel that never reaches
     * terminal count is a machine with an unknown counter, not a machine to
     * hang on. */
    for (spins = 0; spins < 10000000u; spins++) {
        if ((pc_in8(PIT_GATE2) & OUT2_HIGH) != 0) {
            break;
        }
    }

    pc_out8(PIT_GATE2, gate);
}

/*
 * Whether channel two still reaches its count: a millisecond asked for, and
 * twenty on the TSC allowed. Only once the TSC is measured - it is the clock
 * this checks the other one against.
 */
bool pc_pit_counts(void)
{
    uint64_t until;
    uint8_t gate;
    bool counted = false;

    if (tsc_hz == 0) {
        return true;                    /* nothing to check it against */
    }

    pc_out8(PIT_COMMAND, 0xB0);
    pc_out8(PIT_CHANNEL2, (uint8_t)((PIT_HZ / 1000u) & 0xFF));
    pc_out8(PIT_CHANNEL2, (uint8_t)((PIT_HZ / 1000u) >> 8));

    gate = (uint8_t)(pc_in8(PIT_GATE2) & ~(GATE2_ON | SPEAKER_ON));
    pc_out8(PIT_GATE2, gate);
    pc_out8(PIT_GATE2, (uint8_t)(gate | GATE2_ON));

    until = rdtsc() + tsc_hz / 50u;

    while (rdtsc() < until) {
        if ((pc_in8(PIT_GATE2) & OUT2_HIGH) != 0) {
            counted = true;
            break;
        }
    }

    pc_out8(PIT_GATE2, gate);
    return counted;
}

/*
 * One interval of channel two, bracketed: it began between `load` - the TSC
 * before the count's last byte is written - and `opened`, after the gate
 * opens, because an 8254 counts from the gate and QEMU's from the byte, and
 * the two reads hold either. It ended between the read before the last look
 * that saw it still counting and the read after the look that saw it done.
 * So it lasted at least `*shortest` and at most `*longest` cycles.
 *
 * False only when the channel never finished, which is a machine to start
 * with an unknown counter rather than one to try again on - ten million
 * looks is ten seconds on silicon.
 */
static bool look(uint32_t divisor, uint64_t *shortest, uint64_t *longest)
{
    uint64_t load, opened, counting, done = 0;
    uint8_t gate;
    unsigned spins;

    /* The gate closed first, so the count cannot start before it opens. */
    gate = (uint8_t)(pc_in8(PIT_GATE2) & ~(GATE2_ON | SPEAKER_ON));
    pc_out8(PIT_GATE2, gate);

    /* Channel 2, low byte then high, mode 0, binary. */
    pc_out8(PIT_COMMAND, 0xB0);
    pc_out8(PIT_CHANNEL2, (uint8_t)(divisor & 0xFF));

    load = rdtsc();
    pc_out8(PIT_CHANNEL2, (uint8_t)(divisor >> 8));
    pc_out8(PIT_GATE2, (uint8_t)(gate | GATE2_ON));
    opened = rdtsc();

    counting = load;

    for (spins = 0; spins < 10000000u; spins++) {
        uint64_t before = rdtsc();

        if ((pc_in8(PIT_GATE2) & OUT2_HIGH) != 0) {
            done = rdtsc();
            break;
        }

        counting = before;
    }

    pc_out8(PIT_GATE2, gate);   /* gate off again */

    if (done == 0) {
        return false;
    }

    /* Seen done before the gate had opened by the TSC's account: an
     * interval that cannot be bounded below, and the widest possible. */
    *shortest = counting > opened ? counting - opened : 0;
    *longest = done - load;
    return true;
}

static uint64_t calibrate(void)
{
    uint32_t divisor = (PIT_HZ * CALIBRATE_MS) / 1000u;
    uint64_t shortest = 0, longest = 0, mid, hz;
    unsigned tries;
    bool clean = false;

    /* The line's beginning before the measuring, so a machine that stops
     * in it says where, and so `run_timer.py` knows when to interrupt it. */
    kputs("timer: the TSC at ");

    for (tries = 1; tries <= CALIBRATE_TRIES; tries++) {
        uint64_t lo, hi;

        if (!look(divisor, &lo, &hi)) {
            kputs("an unknown rate - the 8253's channel two never finished "
                  "counting\n");
            return 0;
        }

        if (longest == 0 || hi - lo < longest - shortest) {
            shortest = lo;
            longest = hi;
        }

        if ((longest - shortest) * CALIBRATE_SLACK <= longest) {
            clean = true;
            break;
        }
    }

    if (tries > CALIBRATE_TRIES) {
        tries = CALIBRATE_TRIES;
    }

    /* The midpoint, over the interval the divisor really is: 11,931 counts
     * of 1.193182 MHz is 9.9993 ms rather than ten. */
    mid = shortest + (longest - shortest) / 2;
    hz = mid * PIT_HZ / divisor;

    kputu((unsigned long)(hz / 1000u));
    kputs(" kHz, within ");
    kputu((unsigned long)((longest - shortest) / 2 * 1000000u / (mid ? mid : 1)));
    kputs(" ppm, against the 8253 in ");
    kputu(tries);
    kputs(tries == 1 ? " try" : " tries");
    kputs(clean ? "\n" : ", none of them uninterrupted - the narrowest kept\n");

    return hz;
}

uint64_t pc_timer_tsc_hz(void)
{
    return tsc_hz;
}

/* The tick's rate, for `ec_tick`'s thirty seconds. */
static unsigned tick_hz = 100;

void hal_timer_init(unsigned hz)
{
    uint32_t divisor;

    if (hz == 0) {
        hz = 100;
    }

    tick_hz = hz;

    /* The TSC first, if ACPI's setup has not measured it already: the local
     * APIC's timer is measured against it (`apic.c`). */
    pc_timer_measure_tsc();

    /*
     * **The local APIC's own timer, when this machine is driving one.**
     *
     * The 8253 is one counter for a whole machine; the local APIC has one
     * inside every processor, which is what a per-core scheduler tick
     * needs and what `docs/smp.md` has been waiting for on this
     * architecture. It is also the only tick a platform that has dropped
     * the legacy chips can offer at all.
     *
     * Calibrated rather than computed, because nothing says how fast the
     * bus clock it counts is - the same reason the TSC is calibrated rather
     * than read from a table. Against the TSC, which was measured against
     * the 8253, and bracketed the same way (`apic.c`).
     *
     * Falling through on failure is deliberate: a local APIC whose timer
     * did not count is a machine that still has a PIT, and a slower tick
     * is better than none.
     */
    if (pc_irq_on_apic() && apic_timer_init(hz)) {
        on_apic_timer = true;
        cpu_set_counter_hz(tsc_hz);
        return;
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
    cpu_set_counter_hz(tsc_hz);
}

unsigned long hal_ticks(void)
{
    return ticks[this_cpu()->index];
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
/*
 * Another processor's tick count - which is how a core that started and is
 * taking interrupts is told apart from one that started and died.
 *
 * It said core zero's was the only one there was, and it was: nothing on this
 * board could start a second core. Each core started now has its own local
 * APIC timer and its own count, written only by that core.
 */
unsigned long hal_ticks_on(unsigned cpu)
{
    return (cpu < NR_CPUS) ? ticks[cpu] : 0;
}

unsigned long hal_ticks_missed(void)
{
    return 0;
}

/* Called from `pic.c` or `apic.c` with interrupts off, on the core whose
 * timer fired: each core has its own count, and only it writes it. */
void pc_timer_interrupt(void)
{
    ticks[this_cpu()->index]++;

    /* The power button and the embedded controller, on one core only: two
     * processors asking the controller at once would interleave their
     * commands on its one pair of ports. */
    if (this_cpu()->index == 0) {
        ec_tick(tick_hz);
    }
}

const char *hal_timer_describe(void)
{
    return on_apic_timer
         ? "the local APIC's own timer, calibrated against the TSC"
         : "the 8253 through a pair of 8259s";
}
