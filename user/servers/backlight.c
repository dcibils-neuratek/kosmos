/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The backlight: read, and raised to a comfortable level at boot.
 *
 * The ThinkPad's screen is too dim and nothing in Kosmos sets a brightness.
 * Its own firmware says why (`docs/thinkpad.md` 8b): the level is the Intel
 * display engine's backlight PWM, and the firmware expects a graphics driver
 * to set it, which Kosmos does not have. So this is that driver's first
 * half - the part that cannot do harm.
 *
 * **It reads before anything writes**, because the three register offsets
 * come from Linux rather than from a datasheet: the Tiger Lake and Ice Lake
 * register manuals document only the utility pin's backlight, which a laptop
 * panel does not use. Two controllers, each a control, a period and an
 * on-time, and the video BIOS says which one the panel is wired to - so both
 * are read, and a controller that is on, with an on-time inside its period,
 * is what confirms the offsets on the machine itself.
 *
 * **And the ThinkPad confirmed them** on 18 September, stick 0.10.81:
 * controller 0 on, period 19393, on-time 6464 - a third, which is "too dim"
 * - and controller 1 off. So this writes now: the on-time of a controller
 * that reads as on and consistent, raised to `BOOT_PERCENT` of its period,
 * and read back. It never dims a screen the firmware left brighter, and it
 * writes nothing to a controller that is off or reads wrong
 * (`backlight_boot_on_time`, tested on the host with those numbers).
 *
 *   find     the board says where the backlight block is   SYS_DEV_FIND
 *   map      that one page, uncached                       SYS_DEV_MAP
 *   read     both controllers, and say what they hold
 *   write    the on-time of the one that is on, if dim, and read it back
 *
 * **No address of its own**, as `powerbutton.c` has none: the graphics
 * device's base and the block's place in it are `hal/pc/devices.c`'s. The
 * offsets here are within the block. On a machine with no Intel graphics -
 * QEMU, either board - it says so once and exits.
 */

#include <stddef.h>
#include <stdint.h>

#include "kosmos.h"
#include "backlight_decode.h"
#include "mmio.h"
#include "say.h"

/* Within the block `DEV_INTEL_BACKLIGHT` names: the first controller's
 * control, period and on-time, and the second's 100h further on. */
#define CONTROLLER_STRIDE   0x100u
#define PWM_CONTROL         0x250u
#define PWM_PERIOD          0x254u
#define PWM_ON_TIME         0x258u
#define CONTROLLERS         2u

/* How bright at boot, as a share of the period: the ThinkPad's firmware
 * leaves a third, and this is what "too dim" is fixed with before any key
 * changes it. */
#define BOOT_PERCENT        80u

static void report(long console, unsigned which,
                   const struct backlight_controller *c)
{
    struct backlight_reading r;
    struct say_line line;

    backlight_decode(c, &r);

    say_begin(&line);
    say_text(&line, "backlight: controller ");
    say_dec(&line, which);

    switch (r.state) {
    case BACKLIGHT_NOT_ANSWERING:
        say_text(&line, " does not answer - every bit reads as one");
        break;

    case BACKLIGHT_OFF:
        say_text(&line, " off");
        break;

    case BACKLIGHT_INCONSISTENT:
        say_text(&line, " on, and its numbers do not hang together");
        break;

    case BACKLIGHT_ON:
        say_text(&line, " on, ");
        say_dec(&line, r.duty_percent);
        say_text(&line, "% of each period");
        break;
    }

    say_text(&line, ": control ");
    say_hex(&line, c->control, 8);
    say_text(&line, ", period ");
    say_dec(&line, c->period);
    say_text(&line, ", on-time ");
    say_dec(&line, c->on_time);
    say_text(&line, r.active_low ? ", polarity inverted" : ", polarity normal");
    say_send(console, &line);
}

void backlight_server(long console)
{
    struct dev_info dev;
    struct say_line line;
    uintptr_t base;
    unsigned i;
    long mapped;

    if (kosmos_dev_find(DEV_INTEL_BACKLIGHT, 0, &dev) != 0) {
        say(console, "backlight: no Intel graphics on this machine - "
                     "nothing to read\n");
        kosmos_exit(0);
    }

    mapped = kosmos_dev_map((unsigned long)dev.base, 1);

    if (mapped < 0) {
        say(console, "backlight: the backlight registers could not be "
                     "mapped\n");
        kosmos_exit(1);
    }

    base = (uintptr_t)mapped;

    say_begin(&line);
    say_text(&line, "backlight: Intel graphics at 00:");
    say_hex(&line, (dev.where >> 3) & 0x1fu, 2);
    say_text(&line, ".");
    say_dec(&line, dev.where & 0x7u);
    say_text(&line, ", its backlight registers at ");
    say_hex(&line, (unsigned long)dev.base, 16);    /* all of it: ten digits
                                                     * once hid a top half
                                                     * that was not one */
    say_send(console, &line);

    for (i = 0; i < CONTROLLERS; i++) {
        struct backlight_controller c;
        uintptr_t at = base + i * CONTROLLER_STRIDE;
        uint32_t on_time, back;

        c.control = mmio_read32(at + PWM_CONTROL);
        c.period  = mmio_read32(at + PWM_PERIOD);
        c.on_time = mmio_read32(at + PWM_ON_TIME);

        report(console, i, &c);

        if (!backlight_boot_on_time(&c, BOOT_PERCENT, &on_time)) {
            continue;
        }

        /*
         * One register, the on-time. The period stays the firmware's and so
         * does the control word: the controller is already running and
         * already drives the panel, so there is nothing to enable - which
         * is also why a wrong offset here could only have been found by the
         * reading above, and was not.
         */
        mmio_write32(at + PWM_ON_TIME, on_time);
        back = mmio_read32(at + PWM_ON_TIME);

        say_begin(&line);
        say_text(&line, "backlight: controller ");
        say_dec(&line, i);
        say_text(&line, back == on_time ? " raised to " : " NOT raised to ");
        say_dec(&line, BOOT_PERCENT);
        say_text(&line, "% at boot: on-time ");
        say_dec(&line, on_time);
        say_text(&line, " of ");
        say_dec(&line, c.period);
        say_text(&line, ", it was ");
        say_dec(&line, c.on_time);
        say_text(&line, ", and it reads back ");
        say_dec(&line, back);
        say_send(console, &line);
    }

    kosmos_exit(0);
}
