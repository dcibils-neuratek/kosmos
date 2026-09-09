/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Which drivers this board takes its input from, and it is not one answer.
 *
 * **The keyboard comes from the i8042 and the pointer from virtio**, which
 * looks like a compromise and is a statement about where this board is
 * going. The machine this is aimed at - `docs/thinkpad.md` - has an i8042
 * with a keyboard and a TrackPoint on it and no virtio anything. The
 * keyboard half of that driver is proven; the auxiliary half sends nothing
 * under emulation for reasons written down in `i8042.c` and not yet
 * understood.
 *
 * So the real keyboard driver is the one exercised here, every time the
 * display harness types anything - which is what stops it rotting while the
 * other half is worked out - and the pointer stays on the device QEMU will
 * actually drive, because eleven display checks need one and a desktop
 * without a pointer is not a desktop.
 *
 * **When the auxiliary port works, one line here changes.** That is the
 * whole reason this file exists rather than the drivers defining the HAL
 * names themselves.
 */

#include <stdbool.h>

#include "hal.h"
#include "i8042.h"
#include "input.h"

bool hal_keyboard_init(void)   { return i8042_keyboard_init(); }
int  keyboard_getchar(void)    { return i8042_getchar(); }
bool keyboard_present(void)    { return i8042_present(); }

bool hal_key_event(unsigned *code, bool *down)
{
    return i8042_key_event(code, down);
}

bool hal_key_held(unsigned code) { return i8042_key_held(code); }

/*
 * The pointer is virtio's, and it has to be initialised even though the
 * keyboard came from elsewhere: `virtio_pointer_init` is what walks the bus
 * and negotiates with the device, and nothing else does it.
 */
bool hal_pointer_init(void)      { return virtio_pointer_init(); }

bool hal_pointer_poll(struct pointer_state *out)
{
    return virtio_pointer_poll(out);
}

/*
 * Either of them, because "has anything arrived" is a question about the
 * machine rather than about a device. Asking only one would let the desktop
 * sleep through a keystroke because the mouse was quiet.
 */
bool hal_input_pending(void)
{
    return i8042_input_pending() || virtio_input_pending();
}

bool hal_input_pending_peek(void)
{
    return i8042_input_pending_peek() || virtio_input_pending_peek();
}

/*
 * Offered to both, for the reason `pic.c` offers every line to every
 * driver: PCI interrupts are shared and the number alone does not say who
 * raised one. The i8042 masks its own lines and drains anyway if one
 * arrives; virtio reads its interrupt-status byte and decides.
 */
void input_interrupt(unsigned line)
{
    i8042_interrupt(line);
    virtio_input_interrupt(line);
}
