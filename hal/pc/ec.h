/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_HAL_PC_EC_H
#define KOSMOS_HAL_PC_EC_H

/*
 * The embedded controller, watched and not touched - `ec.c` says why.
 *
 * `ec_watch_init` once at boot, after the ACPI walk: says what the FADT says
 * about events, whether SCI_EN is set, and whether a controller answers.
 * `ec_watch_tick` on core 0's timer interrupt: a line for each change in the
 * controller's status or a GPE status bit, the first sixty-four.
 */
void ec_watch_init(void);
void ec_watch_tick(void);

#endif
