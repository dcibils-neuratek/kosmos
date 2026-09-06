/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/* How this board looks for input devices, in the words the boot log uses.
 * The driver is shared with the other machine; where it looks is not. */

const char *hal_input_describe(void)
{
    return "Walking the PCI bus; no device tree to read.";
}
