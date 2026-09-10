/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/* The i8042 keyboard controller, under its own names. `input_bind.c` is
 * where a board decides to use them; `i8042.c` says what works and what
 * does not. */
#ifndef KOSMOS_HAL_PC_I8042_H
#define KOSMOS_HAL_PC_I8042_H

#include <stdbool.h>

struct pointer_state;

bool i8042_keyboard_init(void);
int  i8042_getchar(void);
bool i8042_present(void);
bool i8042_key_event(unsigned *code, bool *down);
bool i8042_key_held(unsigned code);

bool i8042_pointer_init(void);
bool i8042_pointer_poll(struct pointer_state *out);

bool i8042_input_pending(void);
bool i8042_input_pending_peek(void);
void i8042_interrupt(unsigned line);

/* Read (0) or set the pointer's units-per-count. See `i8042.c`. */
unsigned i8042_pointer_speed(unsigned scale);

#endif
