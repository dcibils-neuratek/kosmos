/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Which interrupt controller this machine has, decided when it boots.
 *
 * **Not at build time, because the two machines this has to serve differ
 * in exactly this.** QEMU's q35 has both an 8259 pair and an I/O APIC. A
 * ThinkPad has both. A newer UEFI-only laptop may have only the APIC, and
 * Intel has been removing the legacy pair for years - so a kernel that
 * hard-wired the 8259 gets no scheduler tick there, which presents as a
 * boot that prints all twelve stages and then stops, with nothing else
 * visibly wrong.
 *
 * `docs/targets.md` §1 is the rule: drivers group by what the device is,
 * and which one a machine wants is a question its firmware answers at run
 * time. The answer here is one line - did the MADT describe an I/O APIC -
 * and there is no probing, no address guessed because it is usually there.
 *
 * **The 8259 is still initialised first, on a machine that will not use
 * it.** Two reasons, and neither is caution: the local APIC's timer is
 * calibrated against the 8253, which needs the chip in a known state; and
 * the pair has to be *masked* rather than merely ignored, because it is
 * still wired to the same devices and a line nobody acknowledges is a line
 * held down for ever.
 *
 * The same shape as `input_bind.c` and `snd_bind.c`: the board owns the HAL
 * names and the drivers own their own.
 */

#include <stdbool.h>
#include <stddef.h>

#include "apic.h"
#include "hal.h"
#include "pc.h"

static bool on_apic;
static bool forced;

/*
 * `opt/kosmos/irq=pic` forces the legacy path.
 *
 * **So that both paths are tested rather than one being a fallback nobody
 * runs.** A path taken only by machines this project does not own is a path
 * that rots, and the failure would appear on the first old machine somebody
 * tried - which is precisely when there is least to debug with. q35 has
 * both controllers, so both can be exercised on every gate.
 */
static bool forced_to_pic(void)
{
    char value[16];

    if (!hal_boot_option("opt/kosmos/irq", value, sizeof(value))) {
        return false;
    }

    return value[0] == 'p' && value[1] == 'i' && value[2] == 'c';
}

/*
 * **Decided on first use, because devices are probed before this board
 * initialises its interrupt controller.**
 *
 * `kernel/main.c` brings up the display and the input devices at stages six
 * and seven and the controller at stage eleven, which was fine while there
 * was one controller and a device only had to record which line it wanted.
 * It stopped being fine the moment the answer changed what a device is
 * *programmed with*: `pci_enable` switches a device to MSI where it can,
 * and that is only possible on the APIC path - so a sound controller probed
 * at stage seven was asking a question nothing had answered and being told
 * no. It got its interrupts from a line the I/O APIC was not routing, and
 * the tone played on for three times its length because nothing ever
 * retired a period.
 *
 * Reordering the boot would be the other answer and a worse one: the
 * display exists at stage six precisely so that a failure between there and
 * the end is visible on a machine with no serial port, and moving the
 * controller ahead of it would put the riskiest new code before the only
 * instrument there is.
 */
static bool decided;

static void decide(void)
{
    if (decided) {
        return;
    }

    decided = true;

    if (forced_to_pic()) {
        forced = true;
        return;
    }

    on_apic = apic_init();
}

void hal_irq_init(void)
{
    decide();

    /*
     * Always initialised, even on a machine that will not use it: the pair
     * comes out of reset with its vectors overlapping the processor's
     * exceptions, so a stray line would arrive as a fault rather than as an
     * interrupt. `pic_init` moves them clear and applies whatever masks
     * were recorded while devices were being probed.
     */
    pic_init();

    /*
     * **And then silenced, on the APIC path.** It is still wired to the
     * same devices whatever the I/O APIC is doing, and a chip nobody is
     * listening to will still assert a line into a processor - where an
     * interrupt nobody acknowledges is a line held down for ever.
     */
    if (on_apic) {
        pic_silence();
    }
}

bool hal_irq_handle(void)
{
    return on_apic ? apic_handle() : pic_handle();
}

/*
 * **Which numbers a driver outside the kernel may claim, on a PC.**
 *
 * Not the timer, which is this kernel's tick and whose loss would stop the
 * machine scheduling. Not the keyboard or the mouse, which `i8042.c` still
 * drives from in here. Everything else is a PCI link or an MSI, which is
 * exactly what a userland driver is for.
 *
 * The upper bound is the largest IRQ number `pci.c` mints for an MSI; above
 * that there is no source at all.
 */
bool hal_irq_available(unsigned intid)
{
    /*
     * 0 is the tick, 1 the keyboard and 12 the mouse - the three this kernel
     * unmasks for itself, in `timer.c` and `i8042.c`. Written as the numbers
     * they are because that is how those files ask for them; the day one of
     * them gains a name, this gains it too.
     *
     * 2 is not a device at all: it is the wire the slave 8259 hangs on, and
     * masking it would silence every line on the slave.
     */
    if (intid == 0 || intid == 1 || intid == 2 || intid == 12) {
        return false;
    }

    /*
     * The ceiling is the widest a PC's numbering gets here: sixteen ISA
     * lines, the PCI links above them, and the MSI numbers `pci.c` mints on
     * top. Two hundred is comfortably past all of them and far below the
     * vector space, so it catches a wrong number rather than bounding a
     * design.
     */
    return intid < 200u;
}

/*
 * Mask or unmask one, through whichever controller is running.
 *
 * The APIC's version returns without doing anything for an MSI, and that is
 * correct: there is no redirection entry and nothing is asserted after the
 * handler. `kernel/irq.c` masks on every delivery because a level-triggered
 * line would otherwise arrive again before the driver could run; for an MSI
 * both directions are no-ops and the driver's ack changes nothing.
 */
void hal_irq_set_masked(unsigned intid, bool masked)
{
    if (!hal_irq_available(intid)) {
        return;
    }

    if (masked) {
        if (on_apic) {
            apic_mask(intid);
        } else {
            pic_mask(intid);
        }
    } else {
        pc_irq_unmask(intid);
    }
}

void pc_irq_unmask(unsigned irq)
{
    if (on_apic) {
        apic_unmask(irq);
    } else {
        pic_unmask(irq);
    }
}

/*
 * Which of the two, in the words the boot log uses.
 *
 * The same argument every `describe` in this tree makes: on a machine with
 * no serial port the log is the only instrument, and "no tick" looks
 * identical whether the cause is a missing controller, a line routed to an
 * input nothing is on, or a timer that never counted.
 */
const char *hal_irq_describe(void)
{
    if (on_apic) {
        return apic_describe();
    }

    /*
     * **And on the fallback, why.** This printed the 8259's own description
     * whenever the APIC was not running, so on the first real machine the
     * reason the APIC had been refused - kept in `apic.c`, and correct - was
     * never shown to anybody. The machine said "a pair of 8259s" at every
     * boot and nothing about what it had turned down.
     */
    return forced ? "a pair of 8259s, because opt/kosmos/irq=pic asked for "
                    "them"
                  : apic_describe();
}

bool pc_irq_on_apic(void)
{
    decide();

    return on_apic;
}
