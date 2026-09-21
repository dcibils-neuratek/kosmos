/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Where a relative pointer is - and it is the board's, not a driver's.**
 *
 * A TrackPoint and a mouse report how far they moved, never where they are,
 * so somebody has to keep a position. `i8042.c` kept it, because the
 * TrackPoint was the only thing that moved it, and `hal.h` said pointing
 * devices were not merged: a position can come from only one place, and a
 * second device would be a second opinion to choose between. That is true of
 * an *absolute* device - a tablet says where it is - and the virtio tablet
 * still stands alone.
 *
 * **It is not true of two relative ones.** The ThinkPad has a TrackPoint and a
 * USB mouse plugged into it, and both say "this far": there is nothing to
 * choose between. Both movements add into one position, and a button held on
 * either is held. So the position, its range and the speed live here, and
 * every relative device reports into them - the i8042 from its interrupt, and
 * a USB mouse's driver, which is a process, through `SYS_POINTER_MOVE` and
 * `hal_pointer_move`. The window manager asks `hal_pointer_poll` as it always
 * did and cannot tell which device moved it, which is the point.
 *
 * **Down is positive**, which is USB's way round: HID 1.11 5.9 has a report's
 * values increase "from far to near", so a mouse drawn towards the person moves
 * the pointer down the screen, and QEMU's mouse model agrees. PS/2 counts up
 * as positive, so the i8042 turns its count over before calling in - one
 * convention, at the one place the two meet.
 *
 * **Each source keeps its own buttons**, and the pointer reports all of them
 * together. One byte for both would let a TrackPoint packet with nothing held
 * release a button the mouse is holding.
 *
 * One lock, masking interrupts like every lock here: the i8042 moves this from
 * its interrupt while a system call on another core reads or moves it. It
 * nests inside `i8042_lock`, whose drain calls in holding it, and never the
 * other way round - nothing in this file calls a driver.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hal.h"
#include "pointer.h"
#include "spinlock.h"

/*
 * The range, and it is this board's invention rather than anything a device
 * said. `hal.h` asks for the device's own units with the range beside them,
 * and that is what makes a made-up one honest: the window manager scales
 * whatever range it is given. 32767 is the virtio tablet's, so the desktop
 * cannot tell a relative pointer from an absolute one - which is the point.
 */
#define RANGE           32767

/* The buttons `hal.h` names: bit 0 left, bit 1 right. A source's others - a
 * middle button, a device's own - are not the pointer's. */
#define BUTTONS         0x3u

static struct spinlock pointer_lock = SPINLOCK("pointer");

static uint32_t x = RANGE / 2;
static uint32_t y = RANGE / 2;
static uint32_t held[PC_POINTER_SOURCES];
static bool     present[PC_POINTER_SOURCES];
static bool     moved;

/*
 * Units per count, and the one number here that should be decided on the
 * machine rather than in this file.
 *
 * Eight put a count at about half a pixel on a 1920-wide screen, and the
 * paragraph that used to sit beside it said that was "fine for a mouse and
 * probably slow for a TrackPoint". It was right, and the first machine to run
 * it measured it: the range is 32767 across 1920 pixels, so seventeen units to
 * the pixel, and a count moved 0.47 of one. The pointer worked and crawled.
 *
 * Thirty-two is about 1.9 pixels a count there, which is the speed a
 * TrackPoint wants. What decides it is the ratio of this to `RANGE`: the
 * window manager maps the whole range onto the screen, so the limits here and
 * the screen's edges are reached together, and crossing the screen is a fixed
 * number of counts however big the panel is.
 *
 * **It is one number for every relative device, and that is still the next
 * thing.** A TrackPoint is a strain gauge, a mouse a sensor and a touchpad a
 * surface; they want different speeds, and all of them want a curve.
 * `pointer` at the prompt sets this one, through `hal_pointer_speed`.
 */
static unsigned scale = 32;

static uint32_t along(int64_t at)
{
    if (at < 0) {
        return 0;
    }

    return at > RANGE ? (uint32_t)RANGE : (uint32_t)at;
}

void pc_pointer_arrived(enum pc_pointer_source from)
{
    unsigned long flags;

    if ((unsigned)from >= PC_POINTER_SOURCES) {
        return;
    }

    flags = spin_lock(&pointer_lock);
    present[from] = true;
    spin_unlock(&pointer_lock, flags);
}

void pc_pointer_move(enum pc_pointer_source from, int dx, int dy,
                     uint32_t buttons)
{
    unsigned long flags;

    if ((unsigned)from >= PC_POINTER_SOURCES) {
        return;
    }

    flags = spin_lock(&pointer_lock);

    present[from] = true;

    /*
     * Scaled, because a device reports a handful of counts at a time and the
     * range is fifteen bits; in 64 bits, because the count and the speed are
     * both somebody else's to choose. Clamped at the edges, where a movement
     * further out still says something happened - as it did when this lived
     * in the i8042 driver.
     */
    if (dx != 0 || dy != 0) {
        x = along((int64_t)x + (int64_t)dx * scale);
        y = along((int64_t)y + (int64_t)dy * scale);
        moved = true;
    }

    if ((buttons & BUTTONS) != held[from]) {
        uint32_t merged = 0;
        unsigned i;

        held[from] = buttons & BUTTONS;
        moved = true;

        /*
         * **The transition is kept, not just the state.**
         *
         * A press and a release that both happen between two looks leave
         * this state exactly as it started, so a reader that only samples
         * sees nothing at all - which is the click the ThinkPad's desktop
         * was losing. The merged state is recorded as an edge, with the
         * position it happened at, and `hal/pointer_edges.c` has the
         * reasoning.
         *
         * Merged here rather than in the ring, because what a *source*
         * holds is this file's idea and no other file's.
         */
        for (i = 0; i < PC_POINTER_SOURCES; i++) {
            merged |= held[i];
        }

        hal_pointer_edge(x, y, merged);
    }

    spin_unlock(&pointer_lock, flags);
}

bool pc_pointer_read(struct pointer_state *out)
{
    unsigned long flags = spin_lock(&pointer_lock);
    uint32_t buttons = 0;
    bool any = false;
    unsigned i;

    for (i = 0; i < PC_POINTER_SOURCES; i++) {
        buttons |= held[i];
        any = any || present[i];
    }

    if (any) {
        out->x = x;
        out->y = y;
        out->min_x = 0;
        out->max_x = RANGE;
        out->min_y = 0;
        out->max_y = RANGE;
        out->buttons = buttons;
        out->moved = moved ? 1u : 0u;
        moved = false;
    }

    spin_unlock(&pointer_lock, flags);
    return any;
}

bool pc_pointer_moved(void)
{
    unsigned long flags = spin_lock(&pointer_lock);
    bool was = moved;

    spin_unlock(&pointer_lock, flags);
    return was;
}

/*
 * Zero asks without changing anything, which is what lets one program both
 * report and set. Clamped rather than validated: a speed of nought is a
 * pointer that cannot move and a huge one crosses the screen in a count, and
 * neither is worth an error path in a setting somebody is trying out.
 */
unsigned pc_pointer_speed(unsigned units_per_count)
{
    unsigned long flags = spin_lock(&pointer_lock);
    unsigned now;

    if (units_per_count > 0) {
        scale = units_per_count > 512u ? 512u : units_per_count;
    }

    now = scale;
    spin_unlock(&pointer_lock, flags);
    return now;
}
