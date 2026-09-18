/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The backlight: read, and nothing written - yet.
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
 * is what confirms the offsets on the machine itself. The next version
 * writes an on-time to that one.
 *
 *   find     the board says where the backlight block is   SYS_DEV_FIND
 *   map      that one page, uncached                       SYS_DEV_MAP
 *   read     both controllers, and say what they hold
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
    say_text(&line, " - read only, nothing written");
    say_send(console, &line);

    for (i = 0; i < CONTROLLERS; i++) {
        struct backlight_controller c;
        uintptr_t at = base + i * CONTROLLER_STRIDE;

        c.control = mmio_read32(at + PWM_CONTROL);
        c.period  = mmio_read32(at + PWM_PERIOD);
        c.on_time = mmio_read32(at + PWM_ON_TIME);

        report(console, i, &c);
    }

    kosmos_exit(0);
}
