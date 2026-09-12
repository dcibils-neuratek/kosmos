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

/*
 * **The key with three names and one meaning.**
 *
 * Windows on a PC keyboard, Command on an Apple one, Super on most of the
 * free desktops - and one usage as far as the hardware is concerned: HID
 * 0xE3 and 0xE7, which scancode set 1 delivers as `E0 5B` and `E0 5C`.
 *
 * Named for what it *does* rather than for whose logo is printed on it,
 * because that is the only name that stays true across the keyboards this
 * system will meet. A PC keyboard maps Windows to it, an Apple keyboard
 * maps Command, and nothing above the HAL learns the difference - which is
 * the `arch/` versus `hal/` line applied to a key instead of a chip.
 *
 * 125 and 126 are evdev's numbers, so the virtio driver needs no table
 * entry at all: it is handed these already. See the note at the top of
 * `keys.c` about why the two drivers can share one language.
 */
#define KEY_LEFTMETA    125
#define KEY_RIGHTMETA   126

/* The escape sequence a terminal would have sent for a key that is not a
 * character, or NULL. */
const char *hal_key_sequence(unsigned code);

/* The character a key produces, or -1 for a key that says nothing. */
int hal_key_char(unsigned code, bool shift, bool ctrl, bool caps);

/*
 * A key held with Super as one escape sequence, or Super tapped alone when
 * `c` is not positive. `keys.c` has the shape and why it is xterm's.
 */
const char *hal_key_super(int c);

#endif
