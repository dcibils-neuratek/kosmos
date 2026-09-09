/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The virtio input driver, under its own names.
 *
 * **A board binds these to the HAL, rather than the driver being the HAL.**
 * Both boards used to take their keyboard and their pointer from this one
 * file, so it could define `hal_keyboard_init` and the rest directly. The PC
 * has an i8042 now, and a machine that wants its keyboard from one driver
 * and its pointer from another cannot have two files defining the same nine
 * symbols. See `hal/pc/input_bind.c`, which is exactly that machine.
 */
#ifndef KOSMOS_HAL_VIRTIO_INPUT_H
#define KOSMOS_HAL_VIRTIO_INPUT_H

#include <stdbool.h>

struct pointer_state;

bool virtio_keyboard_init(void);
int  virtio_keyboard_getchar(void);
bool virtio_keyboard_present(void);
bool virtio_key_event(unsigned *code, bool *down);
bool virtio_key_held(unsigned code);

bool virtio_pointer_init(void);
bool virtio_pointer_poll(struct pointer_state *out);

bool virtio_input_pending(void);
bool virtio_input_pending_peek(void);
void virtio_input_interrupt(unsigned line);

#endif
