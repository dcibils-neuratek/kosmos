/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_SERVERS_BACKLIGHT_DECODE_H
#define KOSMOS_SERVERS_BACKLIGHT_DECODE_H

/*
 * What an Intel backlight PWM controller's three registers say, for the
 * backlight driver: whether it is on, whether its numbers hang together, and
 * how much of each period the output is driven.
 *
 * Its own file, with no hardware and no system calls in it, for the reason
 * `usb_decode.c` is: QEMU has no Intel graphics, so the only reading this ever
 * gets under QEMU is none at all, and the ThinkPad gives one reading, once.
 * `tools/test_backlightdecode.c` hands it the rest - a device that does not
 * answer, a period of nought, an on-time past its period, the widest numbers
 * the registers hold.
 *
 * **The layout is Linux's, not a datasheet's.** Control carries enable in
 * bit 31 and polarity in bit 29; the period and the on-time are whole
 * registers beside it (`intel_backlight_regs.h`: `BXT_BLC_PWM_ENABLE`,
 * `BXT_BLC_PWM_POLARITY`, `_BXT_BLC_PWM_FREQ1`, `_BXT_BLC_PWM_DUTY1`). No
 * public Intel manual for Tiger Lake documents the south display's backlight
 * (`docs/thinkpad.md` 8b), which is why the driver reads before anything
 * writes, and why this says `INCONSISTENT` rather than guessing.
 */

#include <stdbool.h>
#include <stdint.h>

/* One controller's registers, as read. */
struct backlight_controller {
    uint32_t control;           /* 31 enable, 29 polarity; the rest ignored */
    uint32_t period;            /* the PWM's whole cycle, in its clock */
    uint32_t on_time;           /* the part of it the output is driven */
};

enum backlight_state {
    BACKLIGHT_NOT_ANSWERING,    /* every bit set: nothing decodes there */
    BACKLIGHT_OFF,              /* the PWM is disabled */
    BACKLIGHT_INCONSISTENT,     /* enabled, with no period or an on-time past it */
    BACKLIGHT_ON,               /* enabled, and the numbers hang together */
};

struct backlight_reading {
    enum backlight_state state;
    bool     active_low;        /* the polarity bit, as set */
    unsigned duty_percent;      /* on-time over period, rounded down; ON only */
};

void backlight_decode(const struct backlight_controller *c,
                      struct backlight_reading *out);

/*
 * **The on-time to write at boot, and whether to write it at all.** True,
 * with `*on_time` set, only for a controller that reads as on with numbers
 * that hang together, and whose on-time is below `percent` of its period:
 * this raises a screen the firmware left dim and never dims one it left
 * brighter. `percent` above 100 is 100.
 *
 * The ThinkPad's reading is the test's first case (`testing.md` 18.99):
 * controller 0 on, period 19393, on-time 6464 - a third.
 */
bool backlight_boot_on_time(const struct backlight_controller *c,
                            unsigned percent, uint32_t *on_time);

#endif
