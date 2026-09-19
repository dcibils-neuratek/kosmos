/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_HAL_PC_EC_H
#define KOSMOS_HAL_PC_EC_H

#include <stdbool.h>

/*
 * ACPI mode, the power button and the embedded controller's events - `ec.c`
 * says why and how.
 *
 * `ec_init` once at boot, after the ACPI walk and before the other
 * processors start: switches to ACPI mode and finds the controller.
 * `ec_tick` on core 0's timer interrupt: the power button and the
 * controller's queries, as keys. `ec_key_event` and `ec_input_pending` are
 * the board's second source of keys, after the keyboard.
 */
void ec_init(void);
void ec_tick(void);
bool ec_key_event(unsigned *code, bool *down);
bool ec_input_pending(void);

#endif
