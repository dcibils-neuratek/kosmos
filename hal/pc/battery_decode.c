/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A ThinkPad's battery registers, decoded. `battery_decode.h` has where
 * each one is and where that came from.
 */

#include "battery_decode.h"

#define STATE_PRESENT      0x80u
#define STATE_CHARGING     0x20u
#define STATE_DISCHARGING  0x40u
#define STATE_LEVEL        0x07u        /* all set: not ready; clear: critical */
#define POWER_AC           0x10u

bool battery_decode(const struct battery_raw *raw, struct hal_battery *out)
{
    unsigned level = raw->state & STATE_LEVEL;
    unsigned long percent;

    out->present     = (raw->state & STATE_PRESENT) != 0;
    out->on_ac       = (raw->power & POWER_AC) != 0;
    out->charging    = false;
    out->discharging = false;
    out->critical    = false;
    out->percent     = 0;

    if (!out->present) {
        return true;
    }

    if (level == STATE_LEVEL || raw->full == 0) {
        return false;
    }

    /* `GBST`'s order: charging wins over discharging. */
    out->charging    = (raw->state & STATE_CHARGING) != 0;
    out->discharging = !out->charging
                       && (raw->state & STATE_DISCHARGING) != 0;
    out->critical    = level == 0;

    percent = ((unsigned long)raw->remaining * 100u + raw->full / 2u)
              / raw->full;
    out->percent = percent > 100u ? 100u : (unsigned)percent;

    return true;
}
