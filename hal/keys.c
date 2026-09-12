/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What a key means, in one place, for every keyboard on every board.
 *
 * **One input language.** A key that is not a character becomes the escape
 * sequence a terminal would have sent for it, so the shell's line editor,
 * the window manager and the widget kit all see `ESC [ A` whether it came
 * off a cable, out of a virtio queue, or from the chip on a laptop's
 * mainboard. The alternative is a second set of codes that exist only on
 * real hardware, and then every consumer has to know about both.
 *
 * **Two drivers share these tables, and the reason they can is history.**
 * `hal/virtio/input.c` is handed Linux evdev key codes; `hal/pc/i8042.c`
 * reads scancode set 1 off a PS/2 controller. Those are the same numbers
 * through the whole typing block - evdev's codes were taken from the XT
 * scancodes in the first place, so `KEY_A` is 30 because the original PC
 * keyboard sent 0x1E for A. Everything above 83 diverges, which is what the
 * extended table in the i8042 driver is for.
 *
 * That coincidence is worth stating rather than relying on quietly: it is
 * the whole reason a laptop keyboard needs no keymap of its own.
 */

#include <stdbool.h>
#include <stddef.h>

#include "keys.h"

static const unsigned char keymap_plain[128] = {
    0x00, 0x1b, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,   /*   0  esc 1234567 */
    0x37, 0x38, 0x39, 0x30, 0x2d, 0x3d, 0x08, 0x09,   /*   8  890-= bs tab */
    0x71, 0x77, 0x65, 0x72, 0x74, 0x79, 0x75, 0x69,   /*  16  qwertyui */
    0x6f, 0x70, 0x5b, 0x5d, 0x0a, 0x00, 0x61, 0x73,   /*  24  op[] enter ctrl as */
    0x64, 0x66, 0x67, 0x68, 0x6a, 0x6b, 0x6c, 0x3b,   /*  32  dfghjkl; */
    0x27, 0x60, 0x00, 0x5c, 0x7a, 0x78, 0x63, 0x76,   /*  40  '` shift \ zxcv */
    0x62, 0x6e, 0x6d, 0x2c, 0x2e, 0x2f, 0x00, 0x00,   /*  48  bnm,./ shift */
    0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  56  alt space caps */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  64 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  72 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  80 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  88 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  96 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* 104 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* 112 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* 120 */
};

static const unsigned char keymap_shift[128] = {
    0x00, 0x1b, 0x21, 0x40, 0x23, 0x24, 0x25, 0x5e,   /*   0  esc !@#$%^ */
    0x26, 0x2a, 0x28, 0x29, 0x5f, 0x2b, 0x08, 0x09,   /*   8  &*()_+ bs tab */
    0x51, 0x57, 0x45, 0x52, 0x54, 0x59, 0x55, 0x49,   /*  16  QWERTYUI */
    0x4f, 0x50, 0x7b, 0x7d, 0x0a, 0x00, 0x41, 0x53,   /*  24  OP{} enter ctrl AS */
    0x44, 0x46, 0x47, 0x48, 0x4a, 0x4b, 0x4c, 0x3a,   /*  32  DFGHJKL: */
    0x22, 0x7e, 0x00, 0x7c, 0x5a, 0x58, 0x43, 0x56,   /*  40  "~ shift | ZXCV */
    0x42, 0x4e, 0x4d, 0x3c, 0x3e, 0x3f, 0x00, 0x00,   /*  48  BNM<>? shift */
    0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  56  alt space caps */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  64 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  72 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  80 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  88 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*  96 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* 104 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* 112 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* 120 */
};

/* The escape sequence for a key that is not a character, or NULL. */
const char *hal_key_sequence(unsigned code)
{
    switch (code) {
    case KEY_UP:       return "\x1b[A";
    case KEY_DOWN:     return "\x1b[B";
    case KEY_RIGHT:    return "\x1b[C";
    case KEY_LEFT:     return "\x1b[D";
    case KEY_HOME:     return "\x1b[H";
    case KEY_END:      return "\x1b[F";
    case KEY_PAGEUP:   return "\x1b[5~";
    case KEY_PAGEDOWN: return "\x1b[6~";
    case KEY_DELETE:   return "\x1b[3~";
    default:           return NULL;
    }
}

/*
 * **A key held with Super, as one sequence.**
 *
 * Windows on a PC keyboard, Command on an Apple one - see `keys.h` for why
 * it is named for neither. What matters here is that this system's input
 * language is characters and escape sequences, so a *held* modifier has
 * nowhere to go: the drivers consume shift and control and hand up the
 * character that resulted. Super is not like those - it changes what a key
 * means rather than which character it is - so it needs a sequence of its
 * own, exactly as the arrow keys do.
 *
 * `ESC [ 1 ; 9 x`, which is xterm's shape for a modified key with 9 as the
 * modifier. Following a convention that exists costs nothing and means a
 * terminal on the other end of a cable would understand it; inventing one
 * would have meant explaining it forever.
 *
 * **And Super alone, tapped, is `ESC [ 1 ; 9 ~`.** A modifier that does
 * something by itself is unusual and is what opens the menu, so it is
 * emitted on *release* and only when nothing was pressed in between - which
 * is the difference between tapping the key and using it to hold a
 * combination.
 *
 * The buffer is static and the caller copies before asking again, which is
 * what `hal_key_sequence` above already promises by returning a literal.
 */
static char super_seq[8];

const char *hal_key_super(int c)
{
    super_seq[0] = 0x1b;
    super_seq[1] = '[';
    super_seq[2] = '1';
    super_seq[3] = ';';
    super_seq[4] = '9';
    super_seq[5] = (char)((c > 0) ? c : '~');
    super_seq[6] = 0;

    return super_seq;
}

/*
 * The character a key produces, or -1 for a key that says nothing.
 *
 * Lifted out of `hal/virtio/input.c` unchanged, because the rules are about
 * keyboards rather than about how the bytes arrived: caps lock is not a
 * second shift, and control turns a letter into the control character it
 * names.
 */
int hal_key_char(unsigned code, bool shift, bool ctrl, bool caps)
{
    unsigned char c;

    if (code >= 128) {
        return -1;
    }

    c = shift ? keymap_shift[code] : keymap_plain[code];

    /*
     * Caps lock is not a second shift: it applies to letters and to nothing
     * else, so 1 stays 1 rather than becoming !. Checking the unshifted
     * letter rather than the result is what makes shift and caps together
     * give a lower-case letter, which is what a keyboard does.
     */
    if (caps) {
        unsigned char plain = keymap_plain[code];

        if (plain >= 'a' && plain <= 'z') {
            c = shift ? plain : keymap_shift[code];
        }
    }

    /*
     * Control turns a letter into the control character it names: C becomes
     * 3, D becomes 4, and so on down the first 32 codes. That mapping is not
     * a convention somebody chose here - it is what the ASCII table is
     * arranged for, which is why clearing bit 6 of the upper-case letter is
     * the whole of it.
     *
     * Only letters. Control-1 is not a character, and inventing one would
     * put a byte on the wire that no program is expecting.
     */
    if (ctrl) {
        unsigned char plain = keymap_plain[code];

        if (plain >= 'a' && plain <= 'z') {
            return (int)((plain - 'a') + 1);
        }

        return -1;                  /* control-anything-else says nothing */
    }

    return (c != 0) ? (int)c : -1;
}
