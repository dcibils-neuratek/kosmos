/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_SERVERS_PAD_DECODE_H
#define KOSMOS_SERVERS_PAD_DECODE_H

/*
 * A game controller's report, and the presses a program sees in it.
 *
 * **An Xbox 360 controller is not a HID device.** Its interface is
 * Microsoft's own - class FFh, subclass 5Dh, protocol 01h - and its report
 * has one fixed shape, 20 bytes from endpoint 81h (the layout below, as
 * published from the device's own traffic and matched against Linux's
 * `xpad` driver): a type of 00h and a length of 14h, then
 *
 *   byte 2   D-pad up, down, left, right, Start, Back, left stick, right stick
 *   byte 3   LB, RB, the Xbox button, -, A, B, X, Y
 *   4, 5     left and right trigger, 0 to 255
 *   6..13    left X, left Y, right X, right Y: signed, little-endian, up
 *            and right positive
 *
 * 8BitDo's SN30 Pro USB is one in X-input mode, which is how it arrives on a
 * computer. Its buttons are where a Super Nintendo's are, and X-input names
 * them by where they sit: the bottom one is A.
 *
 * **What a program sees is keys**, evdev's gamepad codes, which say where a
 * button is rather than what is printed on it - `BTN_SOUTH` is the bottom
 * face button whoever made the pad. The left stick is the D-pad as well,
 * past a threshold, and each trigger is a button past half-way: enough for
 * a Super Nintendo, Doom and Quake, and what a pad driver that has no
 * analogue channel yet can say. Analogue values are kept in the state for
 * when there is one.
 *
 * Its own file, with no hardware in it, so `tools/test_paddecode.c` asks it
 * on the host: QEMU has no game controller to plug in.
 */

#include <stdbool.h>
#include <stdint.h>

/* One bit per button a program sees, in the order `pad_codes` names them. */
enum pad_bit {
    PAD_SOUTH, PAD_EAST, PAD_WEST, PAD_NORTH,
    PAD_TL, PAD_TR, PAD_TL2, PAD_TR2,
    PAD_SELECT, PAD_START, PAD_MODE, PAD_THUMBL, PAD_THUMBR,
    PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
    PAD_BITS
};

/* Each bit's evdev code: BTN_SOUTH 130h to BTN_THUMBR 13Eh, BTN_DPAD_UP
 * 220h to BTN_DPAD_RIGHT 223h - the numbers in Linux's
 * `input-event-codes.h`, which every code in this system follows. */
extern const uint16_t pad_codes[PAD_BITS];

/* A short name for each, for the driver's log. */
extern const char *const pad_names[PAD_BITS];

struct pad_state {
    uint32_t buttons;           /* the pad's own, as `enum pad_bit` bits */
    int16_t  lx, ly, rx, ry;    /* up and right positive */
    uint8_t  lt, rt;
};

/*
 * An Xbox 360 report into `out`. False for anything that is not an input
 * report - a type other than 00h, a length other than 14h, or fewer than
 * twenty bytes - which leaves `out` as it was.
 */
bool pad_decode_x360(const uint8_t *report, unsigned length,
                     struct pad_state *out);

/*
 * The buttons a program sees: the pad's, each trigger past half-way, and
 * the left stick as the D-pad. `before` is what this said last time, so a
 * direction the stick holds is let go only when the stick comes back well
 * inside the threshold - a stick resting near it would otherwise press and
 * release the D-pad on every report.
 */
uint32_t pad_pressed(const struct pad_state *s, uint32_t before);

#define PAD_STICK_PRESS    16384        /* half-way, of 32767 */
#define PAD_STICK_RELEASE  12288        /* and let go inside three eighths */
#define PAD_TRIGGER_PRESS  128u

#endif /* KOSMOS_SERVERS_PAD_DECODE_H */
