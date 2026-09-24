/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Which drivers this board takes its input from, and it is not one answer.
 *
 * **The keyboard always comes from the i8042; the pointer from a virtio
 * tablet if the machine has one, and otherwise from every relative device it
 * has, added together.** The machine this is aimed at - `docs/thinkpad.md` -
 * has an i8042 with a keyboard and a TrackPoint on it and no virtio anything,
 * so the real keyboard driver is the one exercised every time a harness
 * types, which is what stops it rotting.
 *
 * The pointer used to be virtio unconditionally, because the auxiliary port
 * sent nothing under emulation. It works now, on the laptop and under QEMU
 * both, so the order below is the whole of the decision: the display
 * harness keeps its tablet because its checks put the pointer at exact
 * coordinates in one step, and `tools/run_x86.py` boots without one to click
 * through the PS/2 mouse a TrackPoint looks like - and through QEMU's USB
 * mouse, which adds to the same pointer.
 *
 * That is why this file exists rather than the drivers defining the HAL
 * names themselves: which device answers is the board's choice, made once.
 */

#include <stdbool.h>

#include "ec.h"
#include "hal.h"
#include "i8042.h"
#include "input.h"
#include "keys.h"
#include "pointer.h"

bool hal_keyboard_init(void)   { return i8042_keyboard_init(); }
bool keyboard_present(void)    { return i8042_present(); }

/*
 * **The chip's characters, then the ones a process pushed.**
 *
 * A key is two things - an event and a character - and until 22 September a
 * pushed key was only ever the first. That was right while the only thing
 * pushing keys was a game pad; it is wrong for the USB keyboard on a
 * machine with no PS/2 port, which is Diego's ThinkCentre M700
 * (`roadmap.md` 5zd-b). `keys.c` makes the characters, through the same
 * tables the chip's keys go through.
 */
int keyboard_getchar(void)
{
    int c = i8042_getchar();

    return c >= 0 ? c : keys_pushed_char();
}

/*
 * **The keyboard first, then the firmware's keys** - the power button and a
 * laptop's brightness keys, which `ec.c` turns from ACPI events into key
 * codes. A key is a key wherever it came from, the same sentence
 * `hal_getchar` says of characters.
 */
bool hal_key_event(unsigned *code, bool *down)
{
    return i8042_key_event(code, down) || ec_key_event(code, down)
        || keys_pushed_event(code, down);
}

bool hal_key_held(unsigned code) { return i8042_key_held(code); }

/*
 * **virtio if this machine has one, and its relative devices if it does
 * not.**
 *
 * The order is not a preference between two devices - it is which machine
 * this is. A virtio tablet exists only under QEMU, so finding one says "an
 * emulator gave me a pointer" and the eleven display checks that need one
 * keep working exactly as they did. Finding none says "this is a real PC",
 * and on a real PC the pointing device is on the auxiliary port of the same
 * i8042 the keyboard is on - and, since a USB mouse was plugged into one,
 * wherever else a driver finds one.
 *
 * **The TrackPoint driver had never run when this order was written.** The
 * board bound the pointer to virtio unconditionally, so on a laptop there
 * was no pointer at all *and* the driver that might have provided one was
 * never asked: the line had to change before anyone could find out whether
 * it worked. It did, after fixes the laptop found - a configuration byte
 * written with the port still disabled, and a controller read too rarely
 * for a device that sends three bytes a report.
 */
bool pc_pointer_on_virtio(void);

static bool on_virtio_pointer;

bool hal_pointer_init(void)
{
    on_virtio_pointer = virtio_pointer_init();

    if (on_virtio_pointer) {
        return true;
    }

    /* The board's pointer is there from now if the TrackPoint answered; a
     * USB mouse makes it be there with its first report. */
    return i8042_pointer_init();
}

/*
 * The tablet's own answer, or the board's position with whatever the i8042
 * holds drained into it first. False only while the machine has neither a
 * tablet nor any relative device: a USB mouse plugged into a PC with no
 * TrackPoint is a pointer from its first report, and the window manager,
 * which asks on every pass, finds it there.
 */
bool hal_pointer_poll(struct pointer_state *out)
{
    if (on_virtio_pointer) {
        return virtio_pointer_poll(out);
    }

    (void)i8042_pointer_drain();
    return pc_pointer_read(out);
}

/*
 * A process's device, added to the same position - and refused when that
 * position is a tablet's, which says where it is and has no room for how far.
 */
bool hal_pointer_move(int dx, int dy, int wheel, uint32_t buttons)
{
    if (on_virtio_pointer) {
        return false;
    }

    pc_pointer_move(PC_POINTER_DRIVER, dx, dy, wheel, buttons);
    return true;
}

/*
 * Any of them, because "has anything arrived" is a question about the
 * machine rather than about a device. Asking only one would let the desktop
 * sleep through a keystroke because the mouse was quiet.
 *
 * The i8042 first, because asking it drains it, and that is what puts a
 * TrackPoint's packet into the pointer the next question looks at.
 */
bool hal_input_pending(void)
{
    return i8042_input_pending() || pc_pointer_moved()
        || virtio_input_pending() || ec_input_pending()
        || keys_pushed_pending() || keys_pushed_char_pending();
}

bool hal_input_pending_peek(void)
{
    return i8042_input_pending_peek() || pc_pointer_moved()
        || virtio_input_pending_peek() || ec_input_pending()
        || keys_pushed_pending() || keys_pushed_char_pending();
}

/*
 * Offered to both, for the reason `pic.c` offers every line to every
 * driver: PCI interrupts are shared and the number alone does not say who
 * raised one. The i8042 drains on its own two lines, 1 and 12, and ignores
 * the rest; virtio reads its interrupt-status byte and decides.
 */
void input_interrupt(unsigned line)
{
    i8042_interrupt(line);
    virtio_input_interrupt(line);
}

/*
 * Only the relative pointer has one. virtio's tablet is absolute - it
 * reports where it is, not how far it moved - so there is nothing for a gain
 * to multiply, and answering zero is the honest way to say so.
 */
unsigned hal_pointer_speed(unsigned units_per_count)
{
    return on_virtio_pointer ? 0u : pc_pointer_speed(units_per_count);
}

bool pc_pointer_on_virtio(void)
{
    return on_virtio_pointer;
}
