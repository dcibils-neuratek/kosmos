/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Which drivers this board takes its input from: virtio for both.
 *
 * A pass-through, and the indirection is worth it for what it makes
 * possible on the other board rather than for anything it does here. The
 * PC takes its keyboard from an i8042 and its pointer from virtio, and two
 * drivers cannot both define `hal_keyboard_init` - so the HAL names belong
 * to a board rather than to a driver, and this is this board saying so.
 *
 * `hal/pc/input_bind.c` is the same file with a different answer.
 */

#include <stdbool.h>

#include "hal.h"
#include "input.h"

bool hal_keyboard_init(void)   { return virtio_keyboard_init(); }
int  keyboard_getchar(void)    { return virtio_keyboard_getchar(); }
bool keyboard_present(void)    { return virtio_keyboard_present(); }

bool hal_key_event(unsigned *code, bool *down)
{
    return virtio_key_event(code, down);
}

bool hal_key_held(unsigned code) { return virtio_key_held(code); }

bool hal_pointer_init(void)      { return virtio_pointer_init(); }

bool hal_pointer_poll(struct pointer_state *out)
{
    return virtio_pointer_poll(out);
}

bool hal_input_pending(void)      { return virtio_input_pending(); }
bool hal_input_pending_peek(void) { return virtio_input_pending_peek(); }

void input_interrupt(unsigned line) { virtio_input_interrupt(line); }

/*
 * This board's pointer is a virtio tablet, which is absolute: it says where
 * it is rather than how far it moved, so there is no gain to apply. Zero,
 * which is what `hal.h` asks a board with nothing to say to answer.
 */
unsigned hal_pointer_speed(unsigned units_per_count)
{
    (void)units_per_count;
    return 0;
}
