/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * See `apic.h` for what these two chips are and why both paths exist.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "acpi.h"
#include "apic.h"
#include "apic_decode.h"
#include "hal.h"
#include "mmio.h"
#include "mmu.h"
#include "pc.h"
#include "percpu.h"
#include "virtio.h"

/*------------------------------------------------------------------------
 * The local APIC. Intel SDM volume 3, table 11-1.
 *----------------------------------------------------------------------*/

#define LAPIC_ID            0x020
#define LAPIC_VERSION       0x030
#define LAPIC_TPR           0x080   /* task priority: 0 accepts everything */
#define LAPIC_EOI           0x0b0
#define LAPIC_SVR           0x0f0   /* spurious vector, and the enable bit */
#define LAPIC_ISR           0x100   /* eight registers, 16 bytes apart */
#define LAPIC_ESR           0x280
#define LAPIC_LVT_TIMER     0x320
#define LAPIC_TIMER_INIT    0x380
#define LAPIC_TIMER_CURRENT 0x390
#define LAPIC_TIMER_DIVIDE  0x3e0

/*
 * The interrupt command register, which is how one local APIC interrupts
 * another processor: the destination's id in the top byte of the high half,
 * and writing the low half sends. Intel SDM volume 3, section 11.6.1.
 */
#define LAPIC_ICR_LOW       0x300
#define LAPIC_ICR_HIGH      0x310

#define ICR_FIXED           0x000u
#define ICR_INIT            0x500u
#define ICR_STARTUP         0x600u
#define ICR_SEND_PENDING    (1u << 12)
#define ICR_ASSERT          (1u << 14)  /* must be set for all of these */

/*
 * The vector a core is knocked with. 0x3E because it has a stub and a gate,
 * sits above every line `pci.c` will route - those stop at 55 - and below the
 * spurious vector at 0x3F.
 */
#define WAKE_VECTOR         0x3eu

#define SVR_ENABLE          (1u << 8)

#define LVT_MASKED          (1u << 16)
#define LVT_PERIODIC        (1u << 17)

/*
 * Where the local APIC says which mode it is in: IA32_APIC_BASE, Intel SDM
 * volume 3, chapter 11. `apic_decode.h` has what its bits mean.
 */
#define MSR_APIC_BASE       0x1bu

/*
 * Divide by sixteen, which is the encoding's awkwardness rather than a
 * choice: the field is three bits split across bits 3 and 1:0, so 0b1011 is
 * "by one" and 0b0011 is "by sixteen". Sixteen keeps the count inside 32
 * bits on a fast processor - a 4 GHz bus at 100 Hz would overflow at "by
 * one" - and costs nothing, because the tick is calibrated rather than
 * computed.
 */
#define TIMER_DIVIDE_16     0x3u

/*
 * The vector the processor raises when an interrupt is withdrawn between
 * being sent and being taken.
 *
 * 0x3F rather than a tidier number: some processors require the low four
 * bits of this field to be set, and 0x3F is the highest vector the IDT
 * reaches. It has a stub and a gate like any other, and `apic_handle`
 * recognises it by there being nothing in service.
 */
#define SPURIOUS_VECTOR     0x3fu

/*------------------------------------------------------------------------
 * The I/O APIC. 82093AA datasheet, section 3.2.
 *
 * Two registers rather than a register file: write an index to the select
 * window and the data appears in the other. Every entry is 64 bits and is
 * therefore two of these.
 *----------------------------------------------------------------------*/

#define IOAPIC_SELECT       0x00
#define IOAPIC_WINDOW       0x10

#define IOAPIC_REG_VERSION  0x01
#define IOAPIC_REG_ENTRY    0x10    /* entry n is 0x10 + 2n */

#define ENTRY_MASKED        (1u << 16)
#define ENTRY_LEVEL         (1u << 15)
#define ENTRY_ACTIVE_LOW    (1u << 13)

/*
 * The first vector an interrupt line raises, and it is `PC_IRQ_BASE`
 * because the 8259 path already put IRQ n at 32 + n. Keeping them the same
 * means the IDT, the stubs and everything in `trap.c` do not know or care
 * which controller is running.
 */
#define VECTOR_OF(irq)      (PC_IRQ_BASE + (irq))
#define IRQ_OF(vector)      ((vector) - PC_IRQ_BASE)

#define ISA_LINES           16u

/* Kept in step with `pci.c`, which allocates them. */
#define MSI_IRQ_FIRST       20u
#define OVERRIDE_MAX        16u

static struct {
    bool      present;
    uintptr_t lapic;
    uintptr_t ioapic;
    unsigned  id;               /* this processor's local APIC id */
    unsigned  inputs;           /* how many the I/O APIC has */

    struct acpi_override overrides[OVERRIDE_MAX];
    unsigned  override_count;

    unsigned  hz;
    uint32_t  per_tick;         /* core zero's calibration, for the others */

    /* Each core's local APIC id, which it records for itself as it comes up
     * - the only way to learn it is to be that processor. */
    uint32_t  ids[NR_CPUS];
    bool      id_known[NR_CPUS];
} apic;

static const char *description = "a pair of 8259s, because ACPI described "
                                 "no I/O APIC";

/*
 * A model-specific register, read the way `hal/pc/timer.c` reads the time
 * stamp counter: one instruction, and nothing to map.
 */
static uint64_t read_msr(uint32_t msr)
{
    uint32_t lo, hi;

    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));

    return ((uint64_t)hi << 32) | lo;
}

static uint32_t lapic_read(unsigned reg)
{
    return mmio_read32(apic.lapic + reg);
}

static void lapic_write(unsigned reg, uint32_t value)
{
    mmio_write32(apic.lapic + reg, value);
}

static uint32_t ioapic_read(unsigned reg)
{
    mmio_write32(apic.ioapic + IOAPIC_SELECT, reg);

    return mmio_read32(apic.ioapic + IOAPIC_WINDOW);
}

static void ioapic_write(unsigned reg, uint32_t value)
{
    mmio_write32(apic.ioapic + IOAPIC_SELECT, reg);
    mmio_write32(apic.ioapic + IOAPIC_WINDOW, value);
}

/*
 * Which I/O APIC input an ISA interrupt is really on, and how it is wired.
 *
 * The identity mapping is the default and the overrides are the exceptions
 * the firmware announced. `acpi.h` has why this cannot be assumed: the
 * timer is usually IRQ 0 to everybody and input 2 to the chipset, and a
 * driver that programmed input 0 would unmask a line nothing is attached
 * to and never see a tick.
 */
static unsigned input_of(unsigned irq, uint32_t *extra)
{
    unsigned i;

    *extra = 0;

    for (i = 0; i < apic.override_count; i++) {
        if (apic.overrides[i].source != irq) {
            continue;
        }

        /*
         * Bits 1:0 are the polarity and 3:2 the trigger mode, each with 0
         * meaning "whatever the bus normally does" - which for ISA is
         * active high and edge triggered, so only the explicit values move
         * anything.
         */
        if ((apic.overrides[i].flags & 0x3u) == 0x3u) {
            *extra |= ENTRY_ACTIVE_LOW;
        }

        if (((apic.overrides[i].flags >> 2) & 0x3u) == 0x3u) {
            *extra |= ENTRY_LEVEL;
        }

        return (unsigned)apic.overrides[i].gsi;
    }

    return irq;
}

void apic_unmask(unsigned irq)
{
    uint32_t extra;
    unsigned input;

    if (!apic.present) {
        return;
    }

    /*
     * Above the sixteen legacy lines the number *is* the input: those are
     * the PCI links, computed by `pci.c` rather than named by firmware, and
     * an override table only ever describes ISA sources.
     *
     * PCI is also level triggered and active low, where ISA is edge and
     * active high - and a level-triggered line programmed as edge is an
     * interrupt that arrives once and then never again, because nothing
     * ever sees the transition a second time.
     */
    if (irq >= MSI_IRQ_FIRST) {
        /*
         * An MSI has nothing to unmask. The device writes to the local
         * APIC directly, so there is no I/O APIC entry and no line - which
         * is exactly why `pci.c` reaches for one where it can.
         */
        return;
    }

    if (irq >= ISA_LINES) {
        input = irq;
        extra = ENTRY_LEVEL | ENTRY_ACTIVE_LOW;
    } else {
        input = input_of(irq, &extra);
    }

    if (input >= apic.inputs) {
        return;
    }

    /* The destination first, then the entry that becomes live - so the
     * line is never briefly unmasked and aimed at processor zero by
     * default on a machine where that is not this one. */
    ioapic_write(IOAPIC_REG_ENTRY + input * 2 + 1, apic.id << 24);
    ioapic_write(IOAPIC_REG_ENTRY + input * 2, VECTOR_OF(irq) | extra);
}

static void mask_everything(void)
{
    unsigned i;

    for (i = 0; i < apic.inputs; i++) {
        ioapic_write(IOAPIC_REG_ENTRY + i * 2, ENTRY_MASKED);
        ioapic_write(IOAPIC_REG_ENTRY + i * 2 + 1, 0);
    }
}

/*
 * The vector currently being serviced, or zero.
 *
 * The in-service register is eight 32-bit words describing 256 vectors, and
 * the highest bit set is the one the processor is in. Searched from the top
 * because that is the one that interrupted; anything below it is a lower
 * priority interrupt that has not been taken yet.
 */
static unsigned in_service(void)
{
    int word;

    for (word = 7; word >= 0; word--) {
        uint32_t bits = lapic_read(LAPIC_ISR + (unsigned)word * 0x10);

        if (bits != 0) {
            int bit;

            for (bit = 31; bit >= 0; bit--) {
                if ((bits & (1u << bit)) != 0) {
                    return (unsigned)(word * 32 + bit);
                }
            }
        }
    }

    return 0;
}

bool apic_handle(void)
{
    unsigned vector = in_service();
    unsigned irq;
    bool tick;

    /*
     * Nothing in service is the spurious interrupt, and it is the one case
     * that must **not** be acknowledged: the processor withdrew it before
     * it was taken, so there is no in-service bit to clear and an EOI would
     * clear somebody else's.
     */
    if (vector < PC_IRQ_BASE) {
        return false;
    }

    /*
     * A knock from another core, and acknowledging it is the whole of the
     * handling: the thread it was about is already on this core's runqueue,
     * and the way out of the interrupt is where the scheduler runs.
     */
    if (vector == WAKE_VECTOR) {
        lapic_write(LAPIC_EOI, 0);
        return false;
    }

    irq = IRQ_OF(vector);
    tick = (irq == 0);

    if (tick) {
        pc_timer_interrupt();
    } else {
        /*
         * Offered to every driver, exactly as the 8259 path does and for
         * the same reason: PCI lines are shared, so the number narrows it
         * and each device's own status register settles it.
         */
        input_interrupt(irq);
        snd_interrupt(irq);
        net_interrupt(irq);
        blk_interrupt(irq);
    }

    lapic_write(LAPIC_EOI, 0);

    return tick;
}

/*
 * The tick, calibrated rather than computed.
 *
 * **The local APIC timer counts the bus clock, and nothing says how fast
 * that is.** There is a CPUID leaf on recent processors and there is not on
 * the ones this might meet, so the honest way is the way `timer.c` already
 * calibrates the TSC: run the thing that is known against the thing that is
 * not. The 8253 is still there at this point in the boot - it is what this
 * is replacing, and it has not been silenced yet - so it is what the local
 * APIC is measured against.
 *
 * A machine with no 8253 at all is the case this cannot serve, and it is
 * the case that has not arrived: a platform that drops the PIT keeps the
 * HPET, which is the next thing to measure against when one turns up.
 */
bool apic_timer_init(unsigned hz)
{
    uint32_t before, after, per_tick;

    if (!apic.present || hz == 0) {
        return false;
    }

    apic.hz = hz;

    lapic_write(LAPIC_TIMER_DIVIDE, TIMER_DIVIDE_16);

    /* Free-running from the top while the 8253 measures a known interval. */
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_TIMER_INIT, 0xffffffffu);

    before = lapic_read(LAPIC_TIMER_CURRENT);
    pc_timer_wait_ms(10);
    after = lapic_read(LAPIC_TIMER_CURRENT);

    lapic_write(LAPIC_TIMER_INIT, 0);

    if (before <= after) {
        return false;               /* it did not count */
    }

    /* Ticks in ten milliseconds, scaled to the period asked for. */
    per_tick = (before - after) * 100u / hz;

    if (per_tick == 0) {
        return false;
    }

    lapic_write(LAPIC_LVT_TIMER, VECTOR_OF(0) | LVT_PERIODIC);
    lapic_write(LAPIC_TIMER_INIT, per_tick);

    /* Kept, because every core's local APIC timer counts the same bus
     * clock, and calibrating again on each would take ten milliseconds of
     * the 8253 per core for the same answer. */
    apic.per_tick = per_tick;

    return true;
}

unsigned apic_id(void)
{
    return apic.id;
}

bool apic_present(void)
{
    return apic.present;
}

const char *apic_describe(void)
{
    return description;
}

bool apic_init(void)
{
    uint64_t lapic_base;
    uint64_t ioapic_base;
    uint32_t version;

    /*
     * **The tables are not parsed here, and that is not tidiness.**
     *
     * `find_rsdp` reads the word at 0x40E - the BIOS data area's pointer to
     * the EBDA - and 0x40E is inside page 0, which `mmu_init` leaves
     * unmapped on purpose so that a null dereference faults and says so.
     * So ACPI can only be read *before* the kernel builds its own address
     * space, and `hal/pc/cpus.c` does exactly that at the processor stage.
     *
     * Calling it again from here cost a page fault at stage eleven with
     * `cr2` reading 0x40e, on the one boot path that matters - and it was
     * only reached at all because the early parse had already failed and
     * left nothing cached to short-circuit the second attempt.
     */
    lapic_base = acpi_lapic_base();
    ioapic_base = acpi_ioapic_base();

    /*
     * **Either the firmware reported one or it did not.** No probing, no
     * guessing at 0xFEC00000 because that is where it usually is: a machine
     * that does not describe an I/O APIC in its MADT is a machine this must
     * not program one on.
     */
    if (lapic_base == 0 || ioapic_base == 0) {
        return false;
    }

    /*
     * **Which mode the firmware left the local APIC in, before a single
     * register of it is touched.**
     *
     * This driver speaks the memory-mapped interface, and in x2APIC mode
     * that interface is switched off: reads return all ones and writes
     * vanish. The id would read 0xff, the timer would never count, and the
     * first acknowledgement would go nowhere - a machine that takes one
     * interrupt and then none. Refused instead, with the reason in the boot
     * log.
     *
     * A Linux boot log from the laptop this was written for switches x2APIC
     * on itself, which suggests that firmware hands over in the other mode.
     * A suggestion is not a register, and this is what keeps a machine that
     * does otherwise from hanging.
     */
    switch (lapic_mode_of(read_msr(MSR_APIC_BASE))) {
    case LAPIC_XAPIC:
        break;

    case LAPIC_X2APIC:
        description = "a pair of 8259s, because the firmware left the local "
                      "APIC in x2APIC mode, which this driver does not speak";
        return false;

    default:
        description = "a pair of 8259s, because the local APIC is switched "
                      "off in IA32_APIC_BASE";
        return false;
    }

    apic.lapic = mmu_map_device((uintptr_t)lapic_base, 0x1000);
    apic.ioapic = mmu_map_device((uintptr_t)ioapic_base, 0x1000);

    if (apic.lapic == 0 || apic.ioapic == 0) {
        description = "a pair of 8259s, because the device window had no "
                      "room to map the APICs";
        return false;
    }

    apic.override_count = acpi_overrides(apic.overrides, OVERRIDE_MAX);

    /*
     * **How many inputs - and a hundred and twenty is a real answer.**
     *
     * This refused anything above sixty-four as "an impossible size", on an
     * eight-bit field. The chipset in a ThinkPad T14 Gen 2 reports a hundred
     * and twenty, so the first real machine this ran on never left the 8259
     * pair, and the reason sat in this file's description while the boot
     * log printed the 8259's instead.
     *
     * Every input is masked below whatever the count, and only the ones the
     * override table and `pci.c` name are ever unmasked - all of them under
     * twenty-four. What a check is still for is a chip that is not there,
     * which reads all ones.
     */
    version = ioapic_read(IOAPIC_REG_VERSION);
    apic.inputs = ioapic_inputs(version);

    if (apic.inputs == 0) {
        description = "a pair of 8259s, because nothing answers at the I/O "
                      "APIC's address";
        return false;
    }

    apic.id = (lapic_read(LAPIC_ID) >> 24) & 0xffu;
    apic.ids[0] = apic.id;
    apic.id_known[0] = true;

    apic.present = true;
    mask_everything();

    /* Accept every priority, and switch the thing on. The enable bit lives
     * in the same register as the spurious vector, so this is one write and
     * the order inside it does not matter. */
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_SVR, SPURIOUS_VECTOR | SVR_ENABLE);
    lapic_write(LAPIC_ESR, 0);

    description = "an I/O APIC and the local APIC's own timer";

    return true;
}

void apic_init_here(void)
{
    unsigned cpu = this_cpu()->index;

    if (!apic.present || cpu >= NR_CPUS) {
        return;
    }

    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_SVR, SPURIOUS_VECTOR | SVR_ENABLE);
    lapic_write(LAPIC_ESR, 0);

    apic.ids[cpu] = (lapic_read(LAPIC_ID) >> 24) & 0xffu;
    apic.id_known[cpu] = true;

    /* Visible to every other core from here: `apic_wake` reads both. */
    cpu_publish();
}

void apic_timer_init_here(void)
{
    if (!apic.present || apic.per_tick == 0) {
        return;
    }

    lapic_write(LAPIC_TIMER_DIVIDE, TIMER_DIVIDE_16);
    lapic_write(LAPIC_LVT_TIMER, VECTOR_OF(0) | LVT_PERIODIC);
    lapic_write(LAPIC_TIMER_INIT, apic.per_tick);
}

/*
 * One inter-processor interrupt. Waits, boundedly, for the previous one to
 * leave, because a write to the low half while one is still pending is a
 * message the local APIC is free to lose.
 */
static void send_ipi(uint32_t id, uint32_t low)
{
    unsigned long flags = cpu_interrupts_save();
    unsigned spins;

    for (spins = 0; spins < 100000u
                    && (lapic_read(LAPIC_ICR_LOW) & ICR_SEND_PENDING) != 0;
         spins++) {
        cpu_relax();
    }

    /*
     * The destination, then the command, with interrupts masked across both:
     * an interrupt whose handler sends an IPI of its own would rewrite the
     * destination between them, and this one would go to that core instead
     * - a wake lost with nothing to show for it. AArch64 sends an SGI with
     * one system register write and has no such window.
     */
    lapic_write(LAPIC_ICR_HIGH, id << 24);
    lapic_write(LAPIC_ICR_LOW, low);

    cpu_interrupts_restore(flags);
}

/*
 * The MP start-up sequence, SDM volume 3 section 9.4.4: INIT, ten
 * milliseconds, STARTUP, and a second STARTUP after at least two hundred
 * microseconds. A processor already running when the second arrives is out
 * of the wait-for-SIPI state and ignores it, which is why sending both is
 * safe and is what the manual asks for.
 *
 * The waits are the 8253's, which is still running at this point in the
 * boot and is what `apic_timer_init` was calibrated against.
 */
bool apic_start_processor(uint32_t id, unsigned vector)
{
    if (!apic.present || id > 0xfeu || vector > 0xffu) {
        return false;
    }

    send_ipi(id, ICR_INIT | ICR_ASSERT);
    pc_timer_wait_ms(10);

    send_ipi(id, ICR_STARTUP | ICR_ASSERT | vector);
    pc_timer_wait_ms(1);

    send_ipi(id, ICR_STARTUP | ICR_ASSERT | vector);
    pc_timer_wait_ms(1);

    return true;
}

void apic_wake(unsigned cpu)
{
    if (!apic.present || cpu >= NR_CPUS) {
        return;
    }

    cpu_observe();

    if (!apic.id_known[cpu]) {
        return;     /* that processor has not come up and said who it is */
    }

    send_ipi(apic.ids[cpu], ICR_FIXED | ICR_ASSERT | WAKE_VECTOR);
}
