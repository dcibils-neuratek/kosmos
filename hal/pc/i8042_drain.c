/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The i8042's drain, and the one status that means nobody is there.
 *
 * **A status of 0xff is a bus that nothing drives.** Every bit set says a
 * byte is waiting, from the auxiliary port, with a parity error, a timeout
 * and the input buffer full at once - a controller in that state does not
 * exist, and a port nothing answers reads exactly that. So the drain reads
 * no byte after it: reading one would take 0xff as a byte, and go on doing
 * so up to the bound, every time it is asked.
 *
 * That is what the M700 was doing (`roadmap.md`, FOUND on 29 September):
 * the profiler put the console server at 71% of a processor on an idle
 * desktop, inside the syscalls that drain this controller - on a machine
 * with no PS/2 port, whose firmware answers as one only until the USB
 * driver takes the controller from it.
 */

#include "i8042_drain.h"

#define DATA        0x60
#define STATUS      0x64

#define ST_OUTPUT   0x01            /* a byte is waiting to be read */
#define ST_AUX      0x20            /* that byte came from the aux port */
#define ST_FLOATING 0xff

int i8042_drain_bytes(uint8_t (*in)(uint16_t port),
                      void (*byte)(uint8_t b, bool aux),
                      unsigned bound, unsigned *reads)
{
    unsigned n;

    *reads = 0;

    for (n = 0; n < bound; n++) {
        uint8_t status = in(STATUS);

        (*reads)++;

        if (status == ST_FLOATING) {
            return I8042_DRAIN_FLOATS;
        }

        if ((status & ST_OUTPUT) == 0) {
            return I8042_DRAIN_EMPTY;
        }

        {
            uint8_t b = in(DATA);

            (*reads)++;
            byte(b, (status & ST_AUX) != 0);
        }
    }

    return I8042_DRAIN_BOUND;
}

bool i8042_gone(struct i8042_watch *w, int how)
{
    if (how != I8042_DRAIN_FLOATS) {
        w->floating_in_a_row = 0;
        return false;
    }

    return ++w->floating_in_a_row == I8042_GONE_AFTER;
}
