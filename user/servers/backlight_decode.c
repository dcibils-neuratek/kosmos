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
