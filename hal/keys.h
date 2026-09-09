/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What a key means. See `hal/keys.c` for why one table serves both the
 * virtio keyboard and the i8042 on a real PC.
 */
#ifndef KOSMOS_HAL_KEYS_H
#define KOSMOS_HAL_KEYS_H

#include <stdbool.h>

/*
 * The keys that are not characters.
 *
 * A keymap has one byte per keycode, and an arrow is not one byte: over a
 * serial line it arrives as escape, '[', and a letter, because that is what
 * a terminal sends. On a real keyboard it is a single keycode with no
 * character at all - so with a keymap alone, `keymap_plain[108]` is zero,
 * the key produces nothing, and arrows work over the cable and do nothing
 * in the window.
 *
 * That is exactly what happened, and it hid for a while: every automated
 * check types over the serial line, where arrows had always worked.
 */
#define KEY_HOME        102
#define KEY_UP          103
#define KEY_PAGEUP      104
#define KEY_LEFT        105
#define KEY_RIGHT       106
#define KEY_END         107
#define KEY_DOWN        108
#define KEY_PAGEDOWN    109
#define KEY_DELETE      111

#define KEY_LEFTCTRL    29
#define KEY_LEFTSHIFT   42
#define KEY_RIGHTSHIFT  54
#define KEY_CAPSLOCK    58
#define KEY_RIGHTCTRL   97

/* The escape sequence a terminal would have sent for a key that is not a
 * character, or NULL. */
const char *hal_key_sequence(unsigned code);

/* The character a key produces, or -1 for a key that says nothing. */
int hal_key_char(unsigned code, bool shift, bool ctrl, bool caps);

#endif
