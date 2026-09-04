/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_CONPROTO_H
#define KOSMOS_CONPROTO_H

#include <stdint.h>

/*
 * What you may ask the console, written down.
 *
 * The fifth protocol here to be a definition, and the first with an
 * operation that **does not answer when it is asked**. `read` waits for a
 * line, and a line takes as long as somebody takes to type it.
 *
 * The Lua console solved that by blocking inside the handler and pumping its
 * own mailbox from in there - serving other callers while one waited - which
 * works and costs a re-entrant server and a yield-spin, because there is no
 * UART interrupt to park on. The C one holds the sender instead and replies
 * when the line is finished. Nothing re-enters, nothing spins, and the loop
 * that polls the keyboard is the same loop that answers messages.
 *
 * **One reader at a time**, and that is a rule rather than a limitation:
 * there is one keyboard, two readers would split a line between them, and a
 * second client asking for one is a bug in that client.
 */

#define CON_OP_WRITE    1u
#define CON_OP_READ     2u
#define CON_OP_KEYS     3u
#define CON_OP_WAIT     4u
#define CON_OP_POINTER  5u
#define CON_OP_POLL     6u
#define CON_OP_STAT     7u

#define CON_OK              0u
#define CON_ERR_BUSY        1u   /* the console already has a reader */
#define CON_ERR_NO_POINTER  2u
#define CON_ERR_BAD_OP      3u
/*
 * Nothing here reads a line.
 *
 * A terminal's answer, and the reason this protocol has an error for it at
 * all: a terminal window mounts itself as its child's `/dev/console`, so it
 * must answer every operation the real console answers - including the one
 * it cannot do. It used to reply with the sentence "this terminal cannot be
 * read from yet", which was a string invented by one of the two things that
 * implement this protocol. A number, and whoever shows it to a person picks
 * the words.
 */
#define CON_ERR_NO_READER   4u

/*
 * A write is capped rather than chunked here.
 *
 * The whole of a message is 2048 bytes and a reply has to fit beside the
 * request's shape, so 1024 is what is left with room to spare. The caller
 * splits anything longer - `ns.write` does, and it already had to: the Lua
 * console took its text as a serialised string and a four-kilobyte listing
 * did not fit in a message either. What changes is that the limit is now
 * written down instead of being discovered.
 */
#define CON_TEXT_MAX    1024u
#define CON_KEYS_MAX      64u   /* bytes typed since anyone last asked */
#define CON_EVENTS_MAX    32u   /* key transitions, for the window manager */

struct con_key {
    uint32_t code;
    uint32_t down;
};

struct con_request {
    uint32_t op;
    uint32_t ticks;             /* how long `wait` may sleep */
    uint32_t length;            /* bytes of `text`, for write */
    char     text[CON_TEXT_MAX];
};

struct con_reply {
    uint32_t error;

    uint32_t seen;              /* poll: a Control-C went past */
    uint32_t length;            /* read: bytes of `line` */
    uint32_t nkeys;
    uint32_t nevents;

    /*
     * Where the pointer is, in its own units, **and the range those units
     * run over**.
     *
     * The range is not optional and leaving it out is how this protocol was
     * wrong on its first run: `CLAUDE.md` is explicit that the HAL reports
     * in the device's own units with the range beside them and does not
     * scale, because only the window manager knows how big the screen is.
     * QEMU's absolute pointer reports 0..32767; a reply carrying just `x`
     * and `y` says a number with no unit attached to it.
     */
    uint32_t x, y;
    uint32_t min_x, max_x;
    uint32_t min_y, max_y;
    uint32_t buttons, moved;

    /* stat */
    uint32_t bytes, lines, interrupts, reloads;

    uint8_t        keys[CON_KEYS_MAX];
    struct con_key events[CON_EVENTS_MAX];
    char           line[CON_TEXT_MAX];
};

_Static_assert(sizeof(struct con_request) <= 2048,
               "a console request must fit in one message");
_Static_assert(sizeof(struct con_reply) <= 2048,
               "a console reply must fit in one message - lower CON_TEXT_MAX");

#endif /* KOSMOS_CONPROTO_H */
