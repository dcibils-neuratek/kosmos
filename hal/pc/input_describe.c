/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#include <stdbool.h>
/* How this board looks for input devices, in the words the boot log uses.
 * The driver is shared with the other machine; where it looks is not. */

const char *hal_input_describe(void)
{
    return "The i8042 at 0x60, then the PCI bus for everything else.";
}

const char *hal_keyboard_describe(void)
{
    return "i8042, scancode set 1, polled - the chip a laptop still has";
}

/*
 * **Whichever the binding took**, which is what `input_bind.c` decides and
 * this has to agree with. Saying "i8042 auxiliary port" while the pointer
 * came from virtio would be the same kind of lie the kernel used to tell
 * about the keyboard, one layer down - so it asks rather than asserting.
 *
 * On a machine with no virtio the auxiliary port is what is left, and this
 * line is how you find out whether the TrackPoint answered.
 */
bool pc_pointer_on_virtio(void);

const char *hal_pointer_describe(void)
{
    return pc_pointer_on_virtio()
         ? "virtio-input with absolute axes, reporting 0.."
         : "the i8042 auxiliary port, relative counts made absolute";
}
