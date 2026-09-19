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
 *   serve    `/dev/backlight`: get and set a level, 0 to 256
 *
 * **And then it stays, answering** (`backlightproto.h`), because F5 and F6
 * arrived on 19 September - `hal/pc/ec.c` turns them into keys, and the
 * window manager asks here. One controller is served: the first that reads
 * as on, which the firmware lit because the panel is wired to it. Every
 * `set` is written and read back, and the reply carries what the register
 * holds rather than what was asked for.
 *
 * **It answers on a machine with no backlight too**, with
 * `BACKLIGHT_ERR_NO_DEVICE`, rather than exiting. The endpoint is init's,
 * so a driver that left would leave `/dev/backlight` a name whose calls
 * wait for a receiver that is never coming - and the caller that matters
 * is the window manager's key path, where nothing may wait.
 *
 * **No address of its own**, as `powerbutton.c` has none: the graphics
 * device's base and the block's place in it are `hal/pc/devices.c`'s. The
 * offsets here are within the block.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "backlight_decode.h"
#include "backlightproto.h"
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

/* The controller `/dev/backlight` serves: where its registers are, or 0. */
static uintptr_t lit;

static void read_controller(uintptr_t at, struct backlight_controller *c)
{
    c->control = mmio_read32(at + PWM_CONTROL);
    c->period  = mmio_read32(at + PWM_PERIOD);
    c->on_time = mmio_read32(at + PWM_ON_TIME);
}

/*
 * Found, mapped, read, and raised if dim - what this driver did before it
 * served anything, unchanged except that it remembers which controller is
 * lit. False when there is nothing to serve.
 */
static bool bring_up(long console)
{
    struct dev_info dev;
    struct say_line line;
    uintptr_t base;
    unsigned i;
    long mapped;

    if (kosmos_dev_find(DEV_INTEL_BACKLIGHT, 0, &dev) != 0) {
        say(console, "backlight: no Intel graphics on this machine - "
                     "nothing to read\n");
        return false;
    }

    mapped = kosmos_dev_map((unsigned long)dev.base, 1);

    if (mapped < 0) {
        say(console, "backlight: the backlight registers could not be "
                     "mapped\n");
        return false;
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
        struct backlight_reading r;
        uintptr_t at = base + i * CONTROLLER_STRIDE;
        uint32_t on_time, back;

        read_controller(at, &c);
        report(console, i, &c);

        backlight_decode(&c, &r);

        if (r.state == BACKLIGHT_ON && lit == 0) {
            lit = at;
        }

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

    if (lit == 0) {
        say(console, "backlight: no controller is on - nothing to serve\n");
        return false;
    }

    say_begin(&line);
    say_text(&line, "backlight: serving /dev/backlight, controller ");
    say_dec(&line, (unsigned)((lit - base) / CONTROLLER_STRIDE));
    say_send(console, &line);
    return true;
}

static void answer(const struct message *in, uint64_t sender)
{
    struct message out;
    struct backlight_reply *rep = (struct backlight_reply *)(void *)out.data;
    const struct backlight_request *req =
        (const struct backlight_request *)(const void *)in->data;
    struct backlight_controller c;
    uint32_t on_time;

    memset(&out, 0, sizeof(out));
    out.tag = in->tag;
    out.length = (uint32_t)sizeof(*rep);

    if (in->length < sizeof(*req)
        || (req->op != BACKLIGHT_OP_GET && req->op != BACKLIGHT_OP_SET)) {
        rep->error = BACKLIGHT_ERR_BAD_OP;
        (void)kosmos_reply(sender, &out);
        return;
    }

    if (lit == 0) {
        rep->error = BACKLIGHT_ERR_NO_DEVICE;
        (void)kosmos_reply(sender, &out);
        return;
    }

    read_controller(lit, &c);

    /* A set on a controller that has since stopped reading as on - turned
     * off by the firmware, say - writes nothing, and says so. */
    if (req->op == BACKLIGHT_OP_SET) {
        if (!backlight_on_time_for(&c, req->level, BACKLIGHT_LEVEL_FLOOR,
                                   &on_time)) {
            rep->error = BACKLIGHT_ERR_NO_DEVICE;
            (void)kosmos_reply(sender, &out);
            return;
        }

        mmio_write32(lit + PWM_ON_TIME, on_time);
        read_controller(lit, &c);

        if (c.on_time != on_time) {
            rep->error = BACKLIGHT_ERR_UNCHANGED;
        }
    }

    rep->level = backlight_level(&c);
    (void)kosmos_reply(sender, &out);
}

void backlight_server(long console, long endpoint)
{
    (void)bring_up(console);

    for (;;) {
        struct message msg;
        uint64_t sender = 0;

        if (kosmos_receive(endpoint, &msg, &sender, 0, 0) == 0) {
            answer(&msg, sender);
        }
    }
}
