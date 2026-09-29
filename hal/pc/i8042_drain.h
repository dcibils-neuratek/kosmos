/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_HAL_PC_I8042_DRAIN_H
#define KOSMOS_HAL_PC_I8042_DRAIN_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Emptying the keyboard controller, as a loop over a port reader it is
 * handed - so `tools/test_i8042drain.c` can be the controller, as
 * `apic_decode.c` lets a test be the APIC.
 */

#define I8042_DRAIN_EMPTY   0   /* the controller said it holds nothing */
#define I8042_DRAIN_BOUND   1   /* `bound` bytes read, and it said there were more */
#define I8042_DRAIN_FLOATS  2   /* the status read 0xff: nothing drives the bus */

/*
 * How many drains in a row that read 0xff before the controller is taken to
 * have gone - `hal/pc/i8042.c` has why it is not one.
 */
#define I8042_GONE_AFTER   64u

/*
 * Whether the controller has gone, told one drain's ending at a time: true
 * on the `I8042_GONE_AFTER`th that floated in a row, and never before. Any
 * drain that did not float starts the count again.
 */
struct i8042_watch {
    unsigned floating_in_a_row;
};

bool i8042_gone(struct i8042_watch *w, int how);

/*
 * Whatever the controller holds, up to `bound` bytes, each handed to `byte`
 * with whether it came from the auxiliary port; how it ended, and in
 * `*reads` how many times a port was read.
 */
int i8042_drain_bytes(uint8_t (*in)(uint16_t port),
                      void (*byte)(uint8_t b, bool aux),
                      unsigned bound, unsigned *reads);

#endif
