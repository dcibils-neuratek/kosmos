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
 * **The pointer is virtio's and the keyboard is not**, which is what
 * `input_bind.c` decided and this has to agree with. Saying "i8042
 * auxiliary port" here while the binding takes the pointer from virtio
 * would be the same kind of lie the kernel used to tell about the keyboard,
 * one layer down.
 *
 * It changes the day the auxiliary port delivers.
 */
const char *hal_pointer_describe(void)
{
    return "virtio-input with absolute axes, reporting 0..";
}
