/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * An Intel backlight PWM controller, decoded. `backlight_decode.h` says what
 * and why.
 */

#include "backlight_decode.h"

#define CONTROL_ENABLE    (1u << 31)
#define CONTROL_POLARITY  (1u << 29)

void backlight_decode(const struct backlight_controller *c,
                      struct backlight_reading *out)
{
    out->state = BACKLIGHT_OFF;
    out->active_low = false;
    out->duty_percent = 0;

    /*
     * All ones across the three is what a read returns from an address that
     * nothing decodes - memory decoding off, or the wrong window - and it
     * would otherwise read as enabled, active low, and exactly full.
     */
    if (c->control == 0xFFFFFFFFu && c->period == 0xFFFFFFFFu
        && c->on_time == 0xFFFFFFFFu) {
        out->state = BACKLIGHT_NOT_ANSWERING;
        return;
    }

    out->active_low = (c->control & CONTROL_POLARITY) != 0;

    if ((c->control & CONTROL_ENABLE) == 0) {
        return;
    }

    if (c->period == 0 || c->on_time > c->period) {
        out->state = BACKLIGHT_INCONSISTENT;
        return;
    }

    out->state = BACKLIGHT_ON;

    /* Sixty-four bits, because both registers are thirty-two wide and a
     * hundred times the on-time need not fit in them. */
    out->duty_percent = (unsigned)(((uint64_t)c->on_time * 100u) / c->period);
}

bool backlight_boot_on_time(const struct backlight_controller *c,
                            unsigned percent, uint32_t *on_time)
{
    struct backlight_reading r;
    uint32_t want;

    backlight_decode(c, &r);

    if (r.state != BACKLIGHT_ON) {
        return false;
    }

    if (percent > 100u) {
        percent = 100u;
    }

    want = (uint32_t)(((uint64_t)c->period * percent) / 100u);

    if (want <= c->on_time) {
        return false;
    }

    *on_time = want;
    return true;
}

unsigned backlight_level(const struct backlight_controller *c)
{
    struct backlight_reading r;

    backlight_decode(c, &r);

    if (r.state != BACKLIGHT_ON) {
        return 0;
    }

    return (unsigned)(((uint64_t)c->on_time * 256u + c->period / 2u)
                      / c->period);
}

bool backlight_on_time_for(const struct backlight_controller *c,
                           unsigned level, unsigned floor, uint32_t *on_time)
{
    struct backlight_reading r;

    backlight_decode(c, &r);

    if (r.state != BACKLIGHT_ON) {
        return false;
    }

    if (level < floor) {
        level = floor;
    }

    if (level > 256u) {
        level = 256u;
    }

    *on_time = (uint32_t)(((uint64_t)c->period * level + 128u) / 256u);
    return true;
}
