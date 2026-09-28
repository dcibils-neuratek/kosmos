/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_MIDIPROTO_H
#define KOSMOS_MIDIPROTO_H

/*
 * **`/Devices/midi`** (`roadmap.md` 6zg, `usb.md` §12): MIDI keyboards and
 * controllers, served by the USB driver as `/Devices/camera` is, in the
 * shape both sides compile against - `CLAUDE.md`'s declared struct rather
 * than a table.
 *
 *   LIST   `index`: the device at that place - its id, its name, and its
 *          ports each way, by name - and how many there are.
 *   OPEN   `device` (an id, or `MIDI_EVERY`) and a region of one page, the
 *          capability passed with the request: from now on every event from
 *          that device - or from all of them - is written into the region's
 *          ring. The answer is a handle.
 *   CLOSE  `handle`.
 *   SEND   `device`, `cable` and `length` bytes of MIDI: one or more whole
 *          messages, out to the device's port on that cable.
 *
 * **A device is named by an id, not a place.** The place a device has in
 * LIST moves when another is plugged in or out; its id is given when it
 * arrives and never given to anything else, so an event, a SEND and an OPEN
 * all mean the same keyboard for as long as it is there.
 *
 * **Events travel in a ring, not in messages.** They come because a person
 * played, which `CLAUDE.md`'s rule would let travel as messages; but a
 * player's hands and a program's frame are not in step, and a program asking
 * for events would either wait for them or poll. So the driver writes them
 * into a page the program owns and the program reads them when it draws:
 * `write` is the driver's alone and counts every event ever written, and the
 * program keeps its own place. A program more than a ring behind has lost
 * the oldest - `write` minus its place says how many. It writes its place
 * back into `read`, which is the one field it writes: a listener a whole
 * ring behind has stopped listening, and its place goes to the next program
 * that opens. Each event carries the counter's time it was taken, so a
 * program hears *when* a key went down.
 *
 * **A virtual keyboard**, with `opt/kosmos/midi=virtual`: a device with one
 * port each way, whose SEND on cable 0 comes back as its own events. QEMU
 * has no MIDI device and a real one needs root on the Mac, so this is what
 * lets `/Devices/midi`, `midi.lua` and Groove be held by the gate on both
 * boards - as the camera's test pattern does for `/Devices/camera`.
 */

#include <stdint.h>

enum {
    MIDI_OP_LIST  = 1,
    MIDI_OP_OPEN  = 2,
    MIDI_OP_CLOSE = 3,
    MIDI_OP_SEND  = 4,
};

enum {
    MIDI_OK          = 0,
    MIDI_ERR_NONE    = 1,   /* no device at that place, or with that id */
    MIDI_ERR_REQUEST = 2,   /* not a request this understands */
    MIDI_ERR_REGION  = 3,   /* the region was too small, or not one */
    MIDI_ERR_FULL    = 4,   /* as many are listening as can */
    MIDI_ERR_MESSAGE = 5,   /* the bytes are not whole MIDI messages */
    MIDI_ERR_SEND    = 6,   /* the device did not take them */
    MIDI_ERR_CABLE   = 7,   /* the device has no port on that cable */
};

/* Where a device's events come from. */
enum {
    MIDI_SOURCE_USB     = 1,
    MIDI_SOURCE_VIRTUAL = 2,
};

#define MIDI_EVERY          0xFFFFFFFFu     /* OPEN: every device's events */
#define MIDI_NAME_BYTES     40u
#define MIDI_PORTS_NAMED    4u              /* port names a LIST says, each way */
#define MIDI_SEND_BYTES     40u

struct midi_request {                       /* 64 bytes */
    uint32_t op;
    uint32_t index;                         /* LIST */
    uint32_t device;                        /* OPEN, SEND: an id */
    uint32_t handle;                        /* CLOSE */
    uint8_t  cable;                         /* SEND */
    uint8_t  length;                        /* SEND: how many of `bytes` */
    uint8_t  reserved[6];
    uint8_t  bytes[MIDI_SEND_BYTES];
};

struct midi_reply {                         /* 380 bytes */
    uint32_t error;
    uint32_t devices;                       /* LIST: how many there are */
    uint32_t id;                            /* LIST: the device's id */
    uint32_t handle;                        /* OPEN */
    uint8_t  ins;                           /* LIST: ports it sends on */
    uint8_t  outs;                          /* ...and listens on */
    uint8_t  source;                        /* MIDI_SOURCE_* */
    uint8_t  listening;                     /* LIST: programs listening to it */
    char     name[MIDI_NAME_BYTES];
    char     in_names[MIDI_PORTS_NAMED][MIDI_NAME_BYTES];
    char     out_names[MIDI_PORTS_NAMED][MIDI_NAME_BYTES];
};

/* One event, sixteen bytes. `length` of `bytes` are MIDI. */
#define MIDI_EVENT_SYSEX    0x01u           /* a piece of System Exclusive */

struct midi_ring_event {
    uint64_t counter;                       /* the counter when it was taken */
    uint16_t device;                        /* its id */
    uint8_t  cable;
    uint8_t  length;
    uint8_t  flags;
    uint8_t  bytes[3];
};

/* The ring, one page. */
#define MIDI_RING_SLOTS     255u

struct midi_ring {
    uint32_t write;                         /* events written, all told: the
                                               driver's alone */
    uint32_t read;                          /* the program's place: its alone,
                                               and how the driver knows a
                                               listener has stopped listening */
    uint32_t slots;                         /* MIDI_RING_SLOTS */
    uint32_t reserved;
    struct midi_ring_event events[MIDI_RING_SLOTS];
};

_Static_assert(sizeof(struct midi_request) == 64, "the request is 64 bytes");
_Static_assert(sizeof(struct midi_reply) == 380, "the reply is 380 bytes");
_Static_assert(sizeof(struct midi_ring_event) == 16, "an event is 16 bytes");
_Static_assert(sizeof(struct midi_ring) == 4096, "the ring is a page");

#endif /* KOSMOS_MIDIPROTO_H */
