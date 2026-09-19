/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A ThinkPad's battery registers, decoded on the host.
 *
 * QEMU has no embedded controller, so the only bytes `battery_decode` ever
 * gets in a machine are the ThinkPad's. Every state `GBST` in the T14's
 * DSDT tells apart is here, with the readings it would refuse.
 */

#include <stdio.h>

#include "../hal/pc/battery_decode.h"

static int checks;
static int fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  %s\n", what);
    }
}

static struct battery_raw raw(uint8_t state, uint8_t power,
                              uint16_t remaining, uint16_t full)
{
    struct battery_raw r = { state, power, remaining, full };
    return r;
}

int main(void)
{
    struct hal_battery b;
    struct battery_raw r;

    /* Present, discharging, level bits mid-way, on battery: 4000 of 5000. */
    r = raw(0x80 | 0x40 | 0x03, 0x00, 4000, 5000);
    check(battery_decode(&r, &b) && b.present && b.discharging && !b.charging
          && !b.on_ac && !b.critical && b.percent == 80,
          "4000 of 5000, discharging, was not 80% discharging on battery");

    /* Charging on the charger. */
    r = raw(0x80 | 0x20 | 0x03, 0x10, 2501, 5000);
    check(battery_decode(&r, &b) && b.charging && !b.discharging && b.on_ac
          && b.percent == 50,
          "2501 of 5000, charging, was not 50% charging on AC - rounded "
          "to the nearest");

    /* Both bits: charging wins, as GBST decides. */
    r = raw(0x80 | 0x60 | 0x02, 0x10, 100, 100);
    check(battery_decode(&r, &b) && b.charging && !b.discharging
          && b.percent == 100,
          "charging and discharging both set was not read as charging");

    /* On the charger, full, neither bit: held. */
    r = raw(0x80 | 0x01, 0x10, 5000, 5000);
    check(battery_decode(&r, &b) && !b.charging && !b.discharging && b.on_ac
          && b.percent == 100,
          "a full battery on the charger was not 100% and neither charging "
          "nor discharging");

    /* The level bits clear: critical. */
    r = raw(0x80 | 0x40, 0x00, 150, 5000);
    check(battery_decode(&r, &b) && b.critical && b.percent == 3,
          "the level bits clear was not read as critical at 3%");

    /* Remaining past full - an EC updating one before the other - is 100. */
    r = raw(0x80 | 0x40 | 0x03, 0x00, 5200, 5000);
    check(battery_decode(&r, &b) && b.percent == 100,
          "a remaining capacity past full was not held to 100%");

    /* The widest registers, without overflow. */
    r = raw(0x80 | 0x40 | 0x03, 0x00, 0xFFFF, 0xFFFF);
    check(battery_decode(&r, &b) && b.percent == 100,
          "65535 of 65535 was not 100%");

    /* No battery: believed, and not present. */
    r = raw(0x00, 0x10, 0, 0);
    check(battery_decode(&r, &b) && !b.present && b.on_ac && b.percent == 0,
          "no battery on the charger was not read as absent, on AC");

    /* Refused: not ready, and a full capacity of nought. */
    r = raw(0x80 | 0x07, 0x00, 4000, 5000);
    check(!battery_decode(&r, &b),
          "a battery with its level bits all set - not ready - was believed");

    r = raw(0x80 | 0x40 | 0x03, 0x00, 4000, 0);
    check(!battery_decode(&r, &b),
          "a full capacity of nought was believed rather than refused");

    if (fails) {
        printf("FAIL: %d of %d checks on the ThinkPad's battery registers\n",
               fails, fails + checks);
        return 1;
    }

    printf("PASS: %d checks on the ThinkPad's battery registers (charging, "
           "discharging, full on the charger, critical, rounded to the "
           "nearest percent and held at 100, no battery, and the readings "
           "GBST would not trust refused).\n", checks);
    return 0;
}
