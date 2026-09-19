/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What a ThinkPad's embedded controller says about its battery, decoded.
 *
 * **The layout is the T14 Gen 2i's own DSDT**, read with `iasl -d` from the
 * tables the machine gave on 18 September (`thinkpad.md` 8d): the EC's
 * `ECOR` region, whose fields `_BST`, `GBST` and `AC._PSR` read -
 *
 *   38h  HB0S bits 0-6, HB0A bit 7    battery 0's state, and present
 *   46h  HPAC, bit 4                  the charger is plugged in
 *   81h  HIID                         which page appears at A0h
 *   A0h  SBRC, 16 bits                remaining capacity   (page 0)
 *   A2h  SBFC, 16 bits                full-charge capacity (page 0)
 *
 * and `GBST`'s reading of the state: 20h charging, else 40h discharging;
 * low three bits all set, a battery not ready to be read; all clear,
 * critical. SBRC and SBFC are in the same unit whatever it is - `GBST`
 * multiplies both by ten in milliwatt-hour mode - so their ratio is the
 * charge with no unit to know.
 *
 * Split out of `ec.c` so `tools/test_batterydecode.c` can ask it on the
 * host: QEMU has no embedded controller, so the only machine that ever
 * hands this real bytes is the ThinkPad.
 */
#ifndef KOSMOS_HAL_PC_BATTERY_DECODE_H
#define KOSMOS_HAL_PC_BATTERY_DECODE_H

#include <stdbool.h>
#include <stdint.h>

#include "hal.h"

#define EC_BATTERY_STATE   0x38u
#define EC_POWER           0x46u
#define EC_BATTERY_PAGE    0x81u
#define EC_BATTERY_NOW     0xA0u        /* SBRC, low byte first */
#define EC_BATTERY_FULL    0xA2u        /* SBFC */

struct battery_raw {
    uint8_t  state;             /* 38h */
    uint8_t  power;             /* 46h */
    uint16_t remaining;         /* SBRC */
    uint16_t full;              /* SBFC */
};

/*
 * `out` from `raw`. False when the reading is not to be believed - no
 * battery is fine and returns true with `present` false; a battery the
 * controller has not finished measuring, or a full capacity of nought, is
 * false, and the caller keeps what it had.
 */
bool battery_decode(const struct battery_raw *raw, struct hal_battery *out);

#endif
