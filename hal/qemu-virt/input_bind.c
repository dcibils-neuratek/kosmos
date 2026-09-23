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
#include "keys.h"

bool hal_keyboard_init(void)   { return virtio_keyboard_init(); }
bool keyboard_present(void)    { return virtio_keyboard_present(); }

/* The keyboard's characters, then the ones a process pushed - a key is an
 * event *and* a character, and both have to reach somebody. `hal/pc` says
 * the same sentence for the same reason. */
int keyboard_getchar(void)
{
    int c = virtio_keyboard_getchar();

    return c >= 0 ? c : keys_pushed_char();
}

/* The keyboard's keys, then the keys a process pressed (`hal_key_push`). */
bool hal_key_event(unsigned *code, bool *down)
{
    return virtio_key_event(code, down) || keys_pushed_event(code, down);
}

bool hal_key_held(unsigned code) { return virtio_key_held(code); }

bool hal_pointer_init(void)      { return virtio_pointer_init(); }

bool hal_pointer_poll(struct pointer_state *out)
{
    return virtio_pointer_poll(out);
}

/*
 * Refused: this board's pointer is a virtio tablet, which says where it is,
 * and there is no position of the board's own for a movement to be added to -
 * which is what `hal.h` asks a board with an absolute pointer to answer.
 */
bool hal_pointer_move(int dx, int dy, uint32_t buttons)
{
    (void)dx;
    (void)dy;
    (void)buttons;
    return false;
}

bool hal_input_pending(void)
{
    return virtio_input_pending() || keys_pushed_pending()
        || keys_pushed_char_pending();
}

bool hal_input_pending_peek(void)
{
    return virtio_input_pending_peek() || keys_pushed_pending()
        || keys_pushed_char_pending();
}

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
