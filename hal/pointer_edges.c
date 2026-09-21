/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A button transition is an event, and it is kept until somebody reads it.**
 *
 * The position of a pointer is a *state*: it has no history worth keeping
 * and the newest answer is the only right one, so `hal_pointer_poll`
 * reports where it is now and that is correct. A *button* is not like
 * that. A click is a press and a release, and both of them happened
 * whether or not anyone was looking at the moment they did.
 *
 * Nothing kept them, and it cost a real bug on a real machine. Diego, on
 * the ThinkPad, 21 September: "i found some quircks like the mouse buttons
 * be unrespiosnive under certaun scenarios" - on the desktop and the
 * Deskbar, where the window manager is busiest.
 *
 * `user/servers/console.c` had already worked out the mechanism and
 * written it down above `fill_pointer`: "A key is an event and a position
 * is a state. Keys queue, so reading them a pass late loses nothing - they
 * are all still there. The pointer does not queue." It fixed the half it
 * could - sampling after the wait rather than before, which removed one
 * wake of staleness - and named the half it could not: "a window whose
 * repaint takes a dozen messages: the release then arrives while the
 * manager is draining those". Then the press and the release are both
 * behind the manager, `buttons` reads zero when it finally looks, and the
 * click never existed.
 *
 * So the pointer gets what the keyboard has had since it had a driver:
 * `hal/keys.c` keeps key transitions in a ring and `hal_key_event` takes
 * them one at a time. This is that, for buttons, and it is deliberately
 * the same shape - a reader should not have to learn two ideas about what
 * an input event is.
 *
 * **Shared by both boards**, beside `hal/keys.c` and for its reason: which
 * pointing device a board has is the board's business, and what happens to
 * a transition afterwards is not. The PC merges a TrackPoint and a USB
 * mouse into one state and records an edge when that merged state changes;
 * the virt board has a tablet and records one when the tablet's buttons
 * change. Both then behave identically, which is the only way the window
 * manager can be written once.
 *
 * One lock, masking interrupts like every lock here, because an edge
 * arrives from an interrupt handler and is read by a system call on
 * another core.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hal.h"
#include "spinlock.h"

/*
 * Thirty-two, and the number is chosen rather than round.
 *
 * A click is two edges. A person doing their very worst - a double click
 * in each hand - makes eight in the time the window manager takes one
 * pass, and a pass is a frame. Thirty-two is four times the worst case a
 * person can produce, in a queue that is drained every frame.
 *
 * What it is *not* sized for is a device gone mad, and that is deliberate:
 * a mouse reporting a thousand transitions a second is broken, and the
 * right answer to a broken device is to drop what will not fit and keep
 * going, exactly as `hal/keys.c` refuses a press into a full queue. A
 * queue that grew instead would turn a faulty mouse into a kernel that
 * consumes memory without bound, which is the one thing the pools rule
 * exists to prevent.
 */
#define EDGES 32u

static struct pointer_edge ring[EDGES];
static unsigned head;                   /* where the next one goes */
static unsigned count;                  /* how many are waiting */
static unsigned dropped;                /* and how many did not fit */
static struct spinlock edge_lock;

void hal_pointer_edge(uint32_t x, uint32_t y, uint32_t buttons)
{
    unsigned long flags = spin_lock(&edge_lock);

    if (count == EDGES) {
        /*
         * The oldest goes, not the newest.
         *
         * A full queue means nobody has looked for a long time, and in
         * that case the *recent* transitions are the ones that still
         * describe what the person is doing. Dropping the newest would
         * leave a press in the queue whose release had been thrown away -
         * a button stuck down for ever, which is worse than the bug this
         * file exists to fix.
         */
        count--;                /* the tail is head - count: this frees it */
        dropped++;
    }

    ring[head].x = x;
    ring[head].y = y;
    ring[head].buttons = buttons;
    head = (head + 1u) % EDGES;
    count++;

    spin_unlock(&edge_lock, flags);
}

unsigned hal_pointer_edges(struct pointer_edge *out, unsigned max)
{
    unsigned long flags = spin_lock(&edge_lock);
    unsigned taken = 0;

    while (taken < max && count > 0) {
        out[taken] = ring[(head + EDGES - count) % EDGES];
        count--;
        taken++;
    }

    spin_unlock(&edge_lock, flags);

    return taken;
}

unsigned hal_pointer_edges_dropped(void)
{
    unsigned long flags = spin_lock(&edge_lock);
    unsigned n = dropped;

    dropped = 0;

    spin_unlock(&edge_lock, flags);

    return n;
}
