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

#include "hal.h"
#include "kernel.h"
#include "keys.h"
#include "spinlock.h"

/*
 * **The keys processes press** (`hal_key_push`): a queue, and which codes are
 * down, so a driver that ends holding one can have it let go. Pushed by a
 * syscall on any core and taken by the console server's on any other, so
 * under a lock - which masks interrupts, as every lock here does.
 *
 * Sixty-four events is a second of a game pad being mashed; one that is
 * full refuses the press rather than dropping one already queued, and a
 * release is never refused while there is room for one - the order of the
 * checks below is so a key cannot be held for ever by a full queue.
 */
#define PUSHED 64u

static struct spinlock pushed_lock = SPINLOCK("pushed keys");
static struct { uint16_t code; uint8_t down; } pushed[PUSHED];
static unsigned pushed_head, pushed_tail;
static uint32_t pushed_held[(KEY_PUSH_MOST + 1u) / 32u];

static bool pushed_put(unsigned code, bool down)
{
    unsigned next = (pushed_head + 1u) % PUSHED;

    if (next == pushed_tail) {
        return false;
    }

    pushed[pushed_head].code = (uint16_t)code;
    pushed[pushed_head].down = down ? 1u : 0u;
    pushed_head = next;

    if (down) {
        pushed_held[code / 32u] |= 1u << (code % 32u);
    } else {
        pushed_held[code / 32u] &= ~(1u << (code % 32u));
    }

    return true;
}

/*
 * **And the characters those keys mean**, which is the half this did not
 * have until 22 September.
 *
 * A pushed key became an *event*, which is what the window manager reads,
 * and never a *character*, which is what the console server, the shell and
 * every program reading a line read. That was right while the only thing
 * pushing keys was a game pad, whose buttons are not characters. It is
 * wrong for a keyboard: the ThinkCentre M700 has no PS/2 port, so its USB
 * keyboard is the machine's only one, and without this a key pressed on it
 * moved a window and could not type its own name (`roadmap.md` 5zd-b).
 *
 * **The same three functions the PS/2 driver uses** - `hal_key_sequence`
 * for a key that is not a character, `hal_key_char` for one that is, and
 * `hal_key_super` for a key held with Super - so a pushed key means exactly
 * what the same key on a cable means, which is the whole point of there
 * being one table.
 *
 * **The modifiers are tracked from the pushed stream itself**, because that
 * is where they arrive: a keyboard sends shift as a key like any other. A
 * source that pushes a letter and never a shift gets lower case, which is
 * the truthful answer to what it sent.
 */
#define PUSHED_CHARS 128u

static unsigned char pushed_chars[PUSHED_CHARS];
static unsigned pushed_chars_head, pushed_chars_tail;
static bool pushed_shift, pushed_ctrl, pushed_caps, pushed_super;

/*
 * Whether anything was pressed while Super was held: Super let go with
 * nothing between is a tap, which opens the menu, and the PS/2 and virtio
 * keyboards have always said so (`i8042.c`, `input.c`). A USB keyboard's
 * keys come through here, and here it was never said - so on the M700, whose
 * only keyboard is USB, the Windows key alone did nothing (Diego, 3 October
 * 2026: "super key does not open the kosmos menu").
 */
static bool pushed_super_used;

/*
 * **A held key repeats** (Diego, 3 October 2026, on the M700: "maintaining
 * pressed backspace does not keep deleting letters"). A USB keyboard says
 * which keys are down and never repeats one - repeating is the computer's,
 * and a PS/2 keyboard and QEMU's do it themselves, which is why nobody had
 * met it. So a pushed key that meant a character, held, means it again
 * after `REPEAT_AFTER` and every `REPEAT_EVERY` until it is let go - the
 * character only: the event a game reads is still one key, down and then up.
 *
 * **No timer of its own.** Every interrupt, the timer's 250 a second among
 * them, wakes whoever waits for input when input is pending; a repeat that
 * is due counts as pending (`keys_pushed_char_pending`), and is made when
 * the character is read (`keys_pushed_char`). In processor 0's ticks,
 * which are one clock whichever core pushed the key or reads it.
 *
 * Not a key held with Super: that is a command, and Super and Q held would
 * close one window after another.
 */
#define REPEAT_AFTER  (TICK_HZ / 2)          /* half a second */
#define REPEAT_EVERY  (TICK_HZ / 30)         /* about thirty a second */

static unsigned volatile repeat_code;        /* 0: nothing repeats */
static unsigned long volatile repeat_at;     /* processor 0's tick it is due */

static bool repeat_due(void)
{
    return repeat_code != 0
           && (long)(hal_ticks_on(0) - repeat_at) >= 0;
}

static void pushed_char(unsigned char c)
{
    unsigned next = (pushed_chars_head + 1u) % PUSHED_CHARS;

    if (next != pushed_chars_tail) {
        pushed_chars[pushed_chars_head] = c;
        pushed_chars_head = next;
    }
}

static void pushed_string(const char *s)
{
    while (s != NULL && *s != '\0') {
        pushed_char((unsigned char)*s++);
    }
}

/*
 * What a key means now, as characters - its sequence, its character, or a
 * Super chord - pushed. True when it is what a held key repeats.
 */
static bool pushed_meaning(unsigned code)
{
    char buffer[KEY_SEQUENCE_MAX];
    const char *sequence;
    int c;

    sequence = hal_key_sequence(code, pushed_shift, pushed_ctrl, buffer);

    if (sequence != NULL) {
        pushed_string(sequence);
        return true;
    }

    c = hal_key_char(code, pushed_shift, pushed_ctrl, pushed_caps);

    if (c < 0) {
        return false;
    }

    if (pushed_super) {
        pushed_string(hal_key_super(c, buffer));
        return false;
    }

    pushed_char((unsigned char)c);
    return true;
}

/* Called with the lock held, so what a key means and the event it made
 * cannot be interleaved with another core's. */
static void pushed_typed(unsigned code, bool down)
{
    switch (code) {
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:  pushed_shift = down; return;
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:   pushed_ctrl = down;  return;
    case KEY_CAPSLOCK:    if (down) { pushed_caps = !pushed_caps; } return;
    case KEY_LEFTMETA:
    case KEY_RIGHTMETA:
        if (down) {
            pushed_super = true;
            pushed_super_used = false;
        } else {
            char buffer[KEY_SEQUENCE_MAX];
            bool tapped = pushed_super && !pushed_super_used;

            pushed_super = false;

            if (tapped) {
                pushed_string(hal_key_super(0, buffer));
            }
        }

        return;
    default: break;
    }

    if (down && pushed_super) {
        pushed_super_used = true;
    }

    if (!down) {
        if (code == repeat_code) {
            repeat_code = 0;
        }

        return;
    }

    if (pushed_meaning(code)) {
        repeat_code = code;
        repeat_at = hal_ticks_on(0) + REPEAT_AFTER;
    } else {
        repeat_code = 0;
    }
}

bool hal_key_push(unsigned code, bool down)
{
    unsigned long flags;
    bool took;

    if (code > KEY_PUSH_MOST) {
        return false;
    }

    flags = spin_lock(&pushed_lock);
    took = pushed_put(code, down);

    /*
     * The character even when the event's queue was full: they are read by
     * different things at different rates, and a full event queue is no
     * reason for a letter not to arrive.
     */
    pushed_typed(code, down);
    spin_unlock(&pushed_lock, flags);

    return took;
}

int keys_pushed_char(void)
{
    unsigned long flags = spin_lock(&pushed_lock);
    int c = -1;

    /* A held key's next time, made now that somebody reads - and its next
     * due from now rather than from when it was, so a reader that was busy
     * gets one rather than the burst it missed. */
    if (pushed_chars_head == pushed_chars_tail && repeat_due()) {
        (void)pushed_meaning(repeat_code);
        repeat_at = hal_ticks_on(0) + REPEAT_EVERY;
    }

    if (pushed_chars_head != pushed_chars_tail) {
        c = (int)pushed_chars[pushed_chars_tail];
        pushed_chars_tail = (pushed_chars_tail + 1u) % PUSHED_CHARS;
    }

    spin_unlock(&pushed_lock, flags);
    return c;
}

bool keys_pushed_char_pending(void)
{
    return pushed_chars_head != pushed_chars_tail || repeat_due();
}

bool hal_key_release_all(void)
{
    unsigned long flags = spin_lock(&pushed_lock);
    bool any = false;
    unsigned code;

    repeat_code = 0;

    for (code = 0; code <= KEY_PUSH_MOST; code++) {
        if ((pushed_held[code / 32u] & (1u << (code % 32u))) != 0) {
            any = true;

            /* A full queue loses the release; the bit goes either way, so
             * the next driver's press of the same key is not a no-op. */
            if (!pushed_put(code, false)) {
                pushed_held[code / 32u] &= ~(1u << (code % 32u));
            }
        }
    }

    spin_unlock(&pushed_lock, flags);
    return any;
}

bool keys_pushed_event(unsigned *code, bool *down)
{
    unsigned long flags = spin_lock(&pushed_lock);
    bool got = pushed_head != pushed_tail;

    if (got) {
        *code = pushed[pushed_tail].code;
        *down = pushed[pushed_tail].down != 0;
        pushed_tail = (pushed_tail + 1u) % PUSHED;
    }

    spin_unlock(&pushed_lock, flags);
    return got;
}

bool keys_pushed_pending(void)
{
    return pushed_head != pushed_tail;      /* a hint; the lock decides above */
}

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

/*
 * **The sequence for a key, with Shift and Control inside it**, or NULL for
 * a key that is a character.
 *
 * A modifier held with a key that is not a character used to be dropped
 * here: Shift with an arrow was an arrow, and Control with anything but a
 * letter said nothing at all (`hal_key_char`). So no program could select
 * from the keyboard, and the IDE's Ctrl+/, Ctrl+Space and Ctrl+Enter
 * (`roadmap.md` 6n, step 0) had no way to be typed.
 *
 * **The modifier travels inside the sequence**, which is how Super already
 * does it and for the same reason: this system's input language is a byte
 * stream, and a held key has nowhere else to go. Carried in the key's own
 * bytes it cannot arrive out of order with the key, which a separate event
 * could - the window manager posts a pass's characters before its key
 * transitions.
 *
 * The shapes are xterm's, so a terminal at the far end of a cable would
 * read them too, and `m` is its modifier number, 1 plus 1 for Shift, 2 for
 * Alt and 4 for Control:
 *
 *   an arrow, Home, End            ESC [ A        ESC [ 1 ; m A
 *   Insert, Delete, the page keys  ESC [ 5 ~      ESC [ 5 ; m ~
 *   F1 to F4                       ESC O P        ESC [ 1 ; m P
 *   F5 to F12                      ESC [ 15 ~     ESC [ 15 ; m ~
 *   Shift+Tab                      ESC [ Z
 *   Control with a key that is     ESC [ c ; m u, `c` the key's own
 *   not a letter                   character unshifted - 47 for /
 *
 * The last is the "CSI u" form xterm and kitty agree on, for keys the
 * older forms never had room for. Control with a letter is still the
 * control character it names, so every program that reads Control-S as 19
 * goes on doing so. **Control and Shift with a letter** is CSI u too, the
 * letter unshifted - `ESC [ 102 ; 6 u` for Control-Shift-F, the IDE's Find
 * in Project (7 October): as a control character the Shift was lost, and
 * Control-Shift-F was Control-F.
 */
static const char *sequence_of(char out[KEY_SEQUENCE_MAX], unsigned number,
                               unsigned m, char final, bool ss3)
{
    unsigned at = 0;
    char digits[4];
    unsigned n = 0;

    out[at++] = 0x1b;

    if (ss3 && m == 1) {                 /* F1 to F4, unmodified: ESC O P */
        out[at++] = 'O';
        out[at++] = final;
        out[at] = '\0';
        return out;
    }

    out[at++] = '[';

    /* The key's number, when it has one, or 1 when a modifier needs a
     * first parameter to follow. */
    if (number == 0 && m > 1) {
        number = 1;
    }

    if (number > 0) {
        do {
            digits[n++] = (char)('0' + number % 10u);
            number /= 10u;
        } while (number > 0 && n < sizeof(digits));

        while (n > 0) {
            out[at++] = digits[--n];
        }
    }

    if (m > 1) {
        out[at++] = ';';
        out[at++] = (char)('0' + m);
    }

    out[at++] = final;
    out[at] = '\0';
    return out;
}

const char *hal_key_sequence(unsigned code, bool shift, bool ctrl,
                             char out[KEY_SEQUENCE_MAX])
{
    unsigned m = 1u + (shift ? 1u : 0u) + (ctrl ? 4u : 0u);
    unsigned char plain;

    switch (code) {
    case KEY_UP:       return sequence_of(out, 0, m, 'A', false);
    case KEY_DOWN:     return sequence_of(out, 0, m, 'B', false);
    case KEY_RIGHT:    return sequence_of(out, 0, m, 'C', false);
    case KEY_LEFT:     return sequence_of(out, 0, m, 'D', false);
    case KEY_HOME:     return sequence_of(out, 0, m, 'H', false);
    case KEY_END:      return sequence_of(out, 0, m, 'F', false);
    case KEY_INSERT:   return sequence_of(out, 2, m, '~', false);
    case KEY_DELETE:   return sequence_of(out, 3, m, '~', false);
    case KEY_PAGEUP:   return sequence_of(out, 5, m, '~', false);
    case KEY_PAGEDOWN: return sequence_of(out, 6, m, '~', false);
    case KEY_F11:      return sequence_of(out, 23, m, '~', false);
    case KEY_F12:      return sequence_of(out, 24, m, '~', false);
    default:           break;
    }

    /*
     * F1 to F10, which are consecutive codes and not consecutive numbers:
     * xterm skipped 16 and 22, as the VT220 did, so F6 is 17 and F10 is 21.
     */
    if (code >= KEY_F1 && code <= KEY_F10) {
        static const unsigned char numbers[] = { 0, 0, 0, 0, 15, 17, 18, 19, 20, 21 };
        unsigned i = code - KEY_F1;

        if (i < 4u) {
            return sequence_of(out, 0, m, (char)('P' + i), true);
        }

        return sequence_of(out, numbers[i], m, '~', false);
    }

    if (code == KEY_TAB && shift && !ctrl) {
        return sequence_of(out, 0, 1, 'Z', false);
    }

    /*
     * Control with a key whose character is not a letter: Space, Enter,
     * Tab, a digit, a mark. Enter is 13 here as it is to every terminal
     * that sends this, whatever the table below makes of the key.
     */
    if (ctrl && code < 128u) {
        plain = keymap_plain[code];

        if (plain != 0 && !(plain >= 'a' && plain <= 'z')) {
            return sequence_of(out, (plain == '\n') ? 13u : plain, m, 'u', false);
        }

        if (plain >= 'a' && plain <= 'z' && shift) {
            return sequence_of(out, plain, m, 'u', false);
        }
    }

    return NULL;
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
 * Written into the caller's buffer. It was one static buffer, shared by the
 * PS/2 keyboard's interrupt and a USB keyboard's pushed keys, which on a PC
 * run on whichever cores they like.
 */
const char *hal_key_super(int c, char out[KEY_SEQUENCE_MAX])
{
    out[0] = 0x1b;
    out[1] = '[';
    out[2] = '1';
    out[3] = ';';
    out[4] = '9';
    out[5] = (char)((c > 0) ? c : '~');
    out[6] = 0;

    return out;
}

/*
 * The character a key produces, or -1 for a key that says nothing.
 *
 * Lifted out of `hal/virtio/input.c` unchanged, because the rules are about
 * keyboards rather than about how the bytes arrived: caps lock is not a
 * second shift, and control turns a letter into the control character it
 * names.
 */
/*
 * **The numeric keypad** (Diego, 3 October 2026, a full-size keyboard on the
 * M700): its digits, point and operators, and its Enter, which neither table
 * below had - so a keypad typed nothing, on USB or PS/2. Num Lock taken as on,
 * which is how a keyboard starts; its other half is the arrows and Home and
 * End beside it.
 */
static int keypad_char(unsigned code)
{
    switch (code) {
    case 71: return '7';            /* KEY_KP7 */
    case 72: return '8';
    case 73: return '9';
    case 74: return '-';            /* KEY_KPMINUS */
    case 75: return '4';
    case 76: return '5';
    case 77: return '6';
    case 78: return '+';            /* KEY_KPPLUS */
    case 79: return '1';
    case 80: return '2';
    case 81: return '3';
    case 82: return '0';
    case 83: return '.';            /* KEY_KPDOT */
    case 55: return '*';            /* KEY_KPASTERISK */
    case 98: return '/';            /* KEY_KPSLASH */
    case 96: return '\n';           /* KEY_KPENTER, as Enter is */
    default: return -1;
    }
}

int hal_key_char(unsigned code, bool shift, bool ctrl, bool caps)
{
    unsigned char c;
    int k = keypad_char(code);

    if (k >= 0) {
        return ctrl ? -1 : k;
    }

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
