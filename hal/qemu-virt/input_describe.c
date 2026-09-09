/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/* How this board looks for input devices, in the words the boot log uses.
 * The driver is shared with the other machine; where it looks is not. */

const char *hal_input_describe(void)
{
    return "Scanning thirty-two virtio windows; no PCI bus to walk.";
}

const char *hal_keyboard_describe(void)
{
    return "virtio-input, negotiated and polled like the serial line";
}

const char *hal_pointer_describe(void)
{
    return "virtio-input with absolute axes, reporting 0..";
}
