/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
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
 * "Made absolute here", because it is the one thing about this pointer that
 * is not the hardware's doing. A TrackPoint reports how far it moved; the
 * driver keeps the position and reports a range it invented, which `hal.h`
 * permits so long as the range is stated rather than assumed.
 */
const char *hal_pointer_describe(void)
{
    return "i8042 auxiliary port, relative counts made absolute over 0..";
}
