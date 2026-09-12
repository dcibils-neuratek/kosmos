/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The 8259 interrupt controller, remapped.
 *
 * `hal/qemu-virt/gic.c` is the ARM counterpart and the four jobs match:
 * initialise, unmask what we care about, acknowledge, signal end-of-
 * interrupt. What differs is that the GIC was designed for this and the
 * 8259 was designed in 1976 for a machine with no protected mode.
 *
 * **It must be remapped before interrupts are enabled**, and that is the
 * whole reason this file is not four lines. Out of reset the two chips
 * deliver IRQ 0-15 as vectors 8-15 and 0x70-0x77 - and vectors 8 to 15 are
 * *exceptions*: 8 is double fault, 13 is general protection, 14 is page
 * fault. A timer tick would arrive as a double fault, and the report would
 * be a perfectly accurate description of something that never happened.
 *
 * There is an APIC on anything built since about 1995 and it is what a real
 * system uses. This is here because it needs no ACPI tables to find, and
 * finding those is the whole of that work: a tick that arrives is worth
 * more right now than the right controller.
 */

#include <stdint.h>

#include "irq.h"
#include "hal.h"
#include "pc.h"
#include "virtio.h"

#define PIC1_CMD    0x20
#define PIC1_DATA   0x21
#define PIC2_CMD    0xA0
#define PIC2_DATA   0xA1

#define ICW1_INIT   0x11        /* initialise, and a fourth word follows */
#define ICW4_8086   0x01

#define PIC_EOI     0x20
#define PIC_READ_ISR 0x0B       /* OCW3: the next read gives the ISR */

/*
 * A short wait, by writing to a port nothing uses.
 *
 * The 8259 needs time between the initialisation words on hardware old
 * enough to care, and port 0x80 is the POST diagnostic port: writing to it
 * is harmless and takes about a microsecond. This is the traditional way to
 * do it, and it is traditional because there is no better one.
 */
static inline void settle(void)
{
    pc_out8(0x80, 0);
}

/*
 * Which lines have been asked for, one bit each.
 *
 * Held rather than read back from the chips, because `hal_irq_init` writes
 * the mask registers outright and would otherwise lose whatever a driver
 * opened before it ran. There is one interrupt controller on this machine,
 * and this is written once per device at boot.
 */
static uint16_t wanted;

static void apply_masks(void)
{
    pc_out8(PIC1_DATA, (uint8_t)~(wanted & 0xFF));
    pc_out8(PIC2_DATA, (uint8_t)~((wanted >> 8) & 0xFF));
}

/*
 * Every line masked, for a machine that is about to drive the other
 * controller.
 *
 * **Masked rather than left alone, and that is the whole point.** The 8259
 * pair is still wired to the same devices whatever the I/O APIC is doing,
 * and a chip nobody is listening to will still assert a line into a
 * processor - where an interrupt nobody acknowledges is a line held down
 * for ever. Silencing it is one write per chip and removes the whole class.
 */
void pic_silence(void)
{
    wanted = 0;
    apply_masks();
}

void pic_unmask(unsigned irq)
{
    if (irq > 15) {
        return;
    }

    wanted |= (uint16_t)(1u << irq);

    /*
     * A line on the slave only arrives if IRQ 2 is open, because that is
     * the wire the slave is on. Forgetting it is a device that is silent
     * for no visible reason.
     */
    if (irq >= 8) {
        wanted |= (uint16_t)(1u << 2);
    }

    apply_masks();
}

/*
 * And shut one again.
 *
 * IRQ 2 is left open whatever happens: it is the wire the slave controller
 * is on, not a device, so closing it because a line on the slave was masked
 * would silence every other line on the slave too. The cascade is opened by
 * `pic_unmask` for the same reason and is nobody's to close.
 */
void pic_mask(unsigned irq)
{
    if (irq > 15 || irq == 2) {
        return;
    }

    wanted &= (uint16_t)~(1u << irq);

    apply_masks();
}

void pic_init(void)
{
    pc_out8(PIC1_CMD, ICW1_INIT); settle();
    pc_out8(PIC2_CMD, ICW1_INIT); settle();

    pc_out8(PIC1_DATA, PC_IRQ_BASE);     settle();   /* master at 32 */
    pc_out8(PIC2_DATA, PC_IRQ_BASE + 8); settle();   /* slave at 40 */

    pc_out8(PIC1_DATA, 0x04); settle();   /* the slave is on IRQ 2 */
    pc_out8(PIC2_DATA, 0x02); settle();   /* and this is which line that is */

    pc_out8(PIC1_DATA, ICW4_8086); settle();
    pc_out8(PIC2_DATA, ICW4_8086); settle();

    /*
     * Everything masked but what somebody has already asked for.
     *
     * Masking by default is the same rule the GIC follows, for a sharper
     * reason here: an interrupt nobody acknowledges leaves the 8259 waiting
     * for an EOI it will never get, and it then delivers *nothing at all* -
     * which from the outside looks like a dead machine rather than one
     * unhandled device.
     *
     * **`wanted` is what makes the order stop mattering**, and it took a
     * while to find out that it did. `kmain` brings devices up at stage
     * seven and calls this at stage eleven, so every line a driver had
     * opened was masked again four stages later. The keyboard was found,
     * negotiated and had its buffers offered - and never delivered a key,
     * because the controller was told to stop listening after it was told
     * to start.
     *
     * The other board does not have the problem and could not have shown
     * it: a GIC's enable registers are write-one-to-set, so `hal_irq_init`
     * there configures without ever clearing what is already on. Writing a
     * mask byte is not like that. So this restores rather than resets, and
     * a driver may open its line whenever it likes.
     */
    apply_masks();
}


/*
 * Which interrupt is in service, asked of the controller.
 *
 * The processor already knew - it is the vector it dispatched through, and
 * it is sitting in the trap frame two frames up. Asking again costs two
 * port accesses on the tick path, and the reason to pay it is that
 * `hal_irq_handle` takes no argument.
 *
 * That signature is not an oversight to work around. The ARM side reads the
 * GIC's IAR here for exactly the same purpose, so the two boards do the
 * same thing in the same place, and `arch/x86_64/trap.c` gets to stay
 * ignorant of what a PIC is. The alternative was to widen the HAL to
 * `hal_irq_handle(unsigned)` for one board's convenience - which would make
 * the ARM side invent a number to pass, and `CLAUDE.md` is explicit that
 * the HAL is not to grow ahead of a second real caller.
 */
static unsigned in_service(void)
{
    uint8_t master, slave;

    pc_out8(PIC1_CMD, PIC_READ_ISR);
    master = pc_in8(PIC1_CMD);

    if (master == 0) {
        return 0;                       /* nothing: a spurious edge */
    }

    /*
     * Bit 2 is the slave, not a device. When it is set the real line is on
     * the second chip, so ask that one instead.
     */
    if (master & 0x04) {
        pc_out8(PIC2_CMD, PIC_READ_ISR);
        slave = pc_in8(PIC2_CMD);

        if (slave != 0) {
            unsigned i;

            for (i = 0; i < 8; i++) {
                if (slave & (1u << i)) {
                    return 8 + i;
                }
            }
        }
    }

    {
        unsigned i;

        for (i = 0; i < 8; i++) {
            if (master & (1u << i)) {
                return i;
            }
        }
    }

    return 0;
}

/*
 * End of interrupt, and the slave needs telling separately.
 *
 * An interrupt from 8 upward arrived *through* the master as well - that is
 * what the wire on IRQ 2 means - so both chips are waiting to hear it is
 * finished. Telling only the master gives a machine that takes one
 * interrupt from the second chip and never another.
 */
static void eoi(unsigned irq)
{
    if (irq >= 8) {
        pc_out8(PIC2_CMD, PIC_EOI);
    }

    pc_out8(PIC1_CMD, PIC_EOI);
}

bool pic_handle(void)
{
    unsigned irq = in_service();
    bool tick = (irq == 0);

    if (irq == 0) {
        pc_timer_interrupt();
    } else {
        /*
         * Everything else is a PCI device, and **on this bus a line can
         * belong to more than one of them** - four interrupt pins are
         * shared out among every slot, so two devices answering on the same
         * number is ordinary rather than a fault.
         *
         * So each driver is offered the line and decides from its own
         * interrupt-status byte whether anything happened. The ARM board
         * offers its drivers a window index, where the number alone is
         * enough; here the number narrows it and the device settles it.
         */
        input_interrupt(irq);
        snd_interrupt(irq);
        net_interrupt(irq);
        blk_interrupt(irq);

        /*
         * And a driver in a process. `irq_deliver` masks the line before it
         * returns, which is what keeps a level-triggered source from
         * arriving again before the driver - which cannot run until this
         * handler does - has had a turn.
         */
        (void)irq_deliver(irq);
    }

    eoi(irq);

    return tick;
}
