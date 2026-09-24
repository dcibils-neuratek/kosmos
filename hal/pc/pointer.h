/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The pointer this board keeps when its pointing devices are relative, under
 * the board's own names. `pointer.c` says why the position is the board's and
 * not a driver's; `input_bind.c` is what reports it through `hal.h`.
 */
#ifndef KOSMOS_HAL_PC_POINTER_H
#define KOSMOS_HAL_PC_POINTER_H

#include <stdbool.h>
#include <stdint.h>

struct pointer_state;

/*
 * Which device moved it. Each keeps its own buttons, and the pointer reports
 * all of them together.
 */
enum pc_pointer_source {
    PC_POINTER_AUX,             /* the i8042's auxiliary port: a TrackPoint */
    PC_POINTER_DRIVER,          /* a device whose driver is a process: USB */
    PC_POINTER_SOURCES
};

/* A source that exists before it has moved anything: the pointer is there. */
void     pc_pointer_arrived(enum pc_pointer_source from);

/* Counts moved, right and down positive, and this source's buttons. */
void     pc_pointer_move(enum pc_pointer_source from, int dx, int dy, int wheel,
                         uint32_t buttons);

/* Where it is, if any source has arrived. The look clears `moved`. */
bool     pc_pointer_read(struct pointer_state *out);

/* Whether it moved since the last look, without clearing that. */
bool     pc_pointer_moved(void);

/* Read (0) or set the units one count moves. See `pointer.c`. */
unsigned pc_pointer_speed(unsigned units_per_count);

#endif
