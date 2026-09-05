/*
 * GICv3 on QEMU virt.
 *
 * Verified against QEMU's own device tree rather than remembered: with plain
 * `-M virt` this machine gives a **GICv2** (`arm,cortex-a15-gic`, distributor
 * at 0x08000000 and a memory-mapped CPU interface at 0x08010000). GICv3 has
 * to be asked for with `-M virt,gic-version=3`, which is what the Makefile
 * and tools/run_tests.py pass. Booting this code on the default machine
 * finds no redistributor and no interrupt ever arrives.
 *
 * Addresses from the device tree of `-M virt,gic-version=3`: distributor at
 * 0x08000000, redistributors starting at 0x080a0000.
 *
 * Only what a single core needs to take one PPI. No SPIs, no SGIs, no
 * affinity routing, no priority masking beyond letting everything through.
 * All of that arrives when there is a second interrupt source.
 */

#include <stdint.h>

#include "mmio.h"
#include "hal.h"
#include "qemu-virt.h"

#define GICD_BASE           0x08000000UL
#define GICR_BASE           0x080a0000UL

#define GICD_CTLR           (GICD_BASE + 0x0000)

/*
 * The distributor's per-interrupt arrays, for SPIs.
 *
 * A PPI is private to a core and lives in that core's redistributor; an SPI
 * is shared and lives here. Same three things to set - group, priority,
 * enable - in registers of the same shape, indexed from INTID 0 even though
 * the first thirty-two entries are the redistributor's business and
 * writing them here does nothing.
 */
#define GICD_IGROUPR        (GICD_BASE + 0x0080)
#define GICD_ISENABLER      (GICD_BASE + 0x0100)
#define GICD_IPRIORITYR     (GICD_BASE + 0x0400)
#define GICD_IROUTER        (GICD_BASE + 0x6000)

/*
 * The redistributor is two 64 KB frames per core: RD_base, then SGI_base.
 * Everything to do with an individual PPI lives in the second one. Looking
 * for GICR_ISENABLER0 in the first frame is a long afternoon.
 */
#define GICR_RD_BASE        GICR_BASE
#define GICR_SGI_BASE       (GICR_BASE + 0x10000)

#define GICR_WAKER          (GICR_RD_BASE  + 0x0014)
#define GICR_IGROUPR0       (GICR_SGI_BASE + 0x0080)
#define GICR_ISENABLER0     (GICR_SGI_BASE + 0x0100)
#define GICR_IPRIORITYR     (GICR_SGI_BASE + 0x0400)

/* GICD_CTLR, as seen with a single security state. */
#define GICD_CTLR_ENABLE_G0     (1u << 0)
#define GICD_CTLR_ENABLE_G1     (1u << 1)
#define GICD_CTLR_ARE           (1u << 4)   /* affinity routing */

#define GICR_WAKER_PROCESSOR_SLEEP  (1u << 1)
#define GICR_WAKER_CHILDREN_ASLEEP  (1u << 2)

void hal_irq_init(void)
{
    uint32_t waker;

    /*
     * Affinity routing has to be on before anything else is configured: with
     * ARE clear the GIC presents its legacy GICv2 register layout, and the
     * writes below would land in a different meaning.
     */
    mmio_write32(GICD_CTLR,
                 GICD_CTLR_ARE | GICD_CTLR_ENABLE_G1 | GICD_CTLR_ENABLE_G0);

    /*
     * Wake this core's redistributor. It comes out of reset asleep and
     * forwards nothing; clearing ProcessorSleep starts it, and it answers by
     * clearing ChildrenAsleep when it is really running. Skipping the wait
     * means configuring a redistributor that is still asleep, which succeeds
     * and then delivers no interrupts.
     */
    waker = mmio_read32(GICR_WAKER);
    waker &= ~GICR_WAKER_PROCESSOR_SLEEP;
    mmio_write32(GICR_WAKER, waker);

    while (mmio_read32(GICR_WAKER) & GICR_WAKER_CHILDREN_ASLEEP) {
        /* wait */
    }

    /*
     * The CPU interface is system registers on GICv3, not MMIO, and access
     * to them has to be turned on first. ICC_SRE_EL1.SRE, then an isb,
     * because every ICC_* access after this depends on the write having
     * taken effect.
     */
    __asm__ volatile(
        "mrs x0, icc_sre_el1\n"
        "orr x0, x0, #1\n"
        "msr icc_sre_el1, x0\n"
        "isb\n"
        ::: "x0", "memory");

    /* Let every priority through. There is one interrupt source; masking by
     * priority is a decision for when there are several. */
    __asm__ volatile("msr icc_pmr_el1, %0" : : "r"((uint64_t)0xff));

    /* No preemption grouping. */
    __asm__ volatile("msr icc_bpr1_el1, %0" : : "r"((uint64_t)0));

    /* And finally let Group 1 interrupts reach this core. */
    __asm__ volatile("msr icc_igrpen1_el1, %0" : : "r"((uint64_t)1));
    __asm__ volatile("isb" ::: "memory");
}

void gic_enable_ppi(unsigned intid)
{
    uint32_t group;

    /*
     * A PPI is private to a core, so it is configured in that core's
     * redistributor and not in the distributor. INTIDs 0 to 31 all live in
     * the single 32-bit register at offset 0.
     */
    group = mmio_read32(GICR_IGROUPR0);
    group |= (uint32_t)1 << intid;
    mmio_write32(GICR_IGROUPR0, group);         /* Group 1, non-secure */

    /* One byte of priority per INTID. 0 is the highest. */
    mmio_write32(GICR_IPRIORITYR + (intid & ~3u),
                 (uint32_t)0 << ((intid & 3u) * 8));

    /* Write-one-to-set: a read-modify-write here would race with the GIC. */
    mmio_write32(GICR_ISENABLER0, (uint32_t)1 << intid);
}

/*
 * A shared interrupt, routed to this core.
 *
 * Three registers and one that is easy to forget: with affinity routing on -
 * and `GICD_CTLR_ARE` is set in `hal_irq_init` - an SPI goes nowhere until
 * GICD_IROUTER says which core it is for. The symptom of leaving it out is a
 * device that is enabled, raises its interrupt, and is never taken.
 */
void gic_enable_spi(unsigned intid)
{
    unsigned word = intid / 32;
    unsigned bit = intid % 32;
    uint32_t group;

    group = mmio_read32(GICD_IGROUPR + word * 4);
    group |= (uint32_t)1 << bit;
    mmio_write32(GICD_IGROUPR + word * 4, group);   /* Group 1, non-secure */

    /* One byte of priority per INTID. 0 is the highest, and the timer has
     * it too - nothing here needs an ordering between them. */
    mmio_write32(GICD_IPRIORITYR + (intid & ~3u),
                 (uint32_t)0 << ((intid & 3u) * 8));

    /* Affinity 0.0.0.0, which is the only core there is. Bit 31 clear means
     * "this exact affinity" rather than "any core that can take it". */
    mmio_write32(GICD_IROUTER + intid * 8, 0);
    mmio_write32(GICD_IROUTER + intid * 8 + 4, 0);

    /* Write-one-to-set: a read-modify-write would race with the GIC. */
    mmio_write32(GICD_ISENABLER + word * 4, (uint32_t)1 << bit);
}

unsigned gic_acknowledge(void)
{
    uint64_t intid;

    /* Reading IAR is what acknowledges the interrupt and moves it to active.
     * It has to be paired with a write to EOIR or the interrupt stays active
     * and never fires again. */
    __asm__ volatile("mrs %0, icc_iar1_el1" : "=r"(intid));
    __asm__ volatile("dsb sy" ::: "memory");

    return (unsigned)intid;
}

void gic_end_of_interrupt(unsigned intid)
{
    __asm__ volatile("msr icc_eoir1_el1, %0" : : "r"((uint64_t)intid));
    __asm__ volatile("isb" ::: "memory");
}

void hal_irq_handle(void)
{
    unsigned intid = gic_acknowledge();

    /* The GIC hands back 1023 when the interrupt went away before it was
     * acknowledged. Nothing to service, and nothing to signal. */
    if (intid == GIC_SPURIOUS) {
        return;
    }

    if (intid == TIMER_INTID) {
        timer_interrupt();
    } else if (intid >= VIRTIO_INTID_BASE
               && intid < VIRTIO_INTID_BASE + VIRTIO_MMIO_COUNT) {
        /*
         * A virtio-mmio device. Which one is the INTID minus the base:
         * `virt` maps slot i to SPI 16 + i, and a GIC interrupt ID for an
         * SPI is 32 + the SPI number.
         *
         * Offered to each driver in turn rather than looked up, and every
         * one of them returns immediately unless the slot is its own.
         *
         * **This said a registry was the answer once a third kind of device
         * wanted a line. The network card is the third, and the judgement
         * has not changed** - a registry is a table, a registration call and
         * an indirection, and what it saves is two comparisons on a path
         * that runs when a device has something to say. Recorded rather than
         * quietly ignored, because a threshold that arrives and is stepped
         * over without comment is a threshold nobody trusts next time.
         *
         * What would change it is a *dynamic* set of drivers - a driver
         * loaded at run time cannot be a line in this function - and that is
         * the same milestone as drivers at EL0.
         */
        unsigned slot = intid - VIRTIO_INTID_BASE;

        input_interrupt(slot);
        snd_interrupt(slot);
        net_interrupt(slot);
    }

    /*
     * Anything else is signalled complete and dropped. A registry of
     * handlers is for when there are more kinds than this, which is M11.
     */
    gic_end_of_interrupt(intid);
}
