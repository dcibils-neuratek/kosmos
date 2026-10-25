/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DRIVERS_BLUETOOTH_BTSIM_H
#define KOSMOS_DRIVERS_BLUETOOTH_BTSIM_H

/*
 * **A Bluetooth controller in software** (`bluetooth.md` B2): what the
 * host stack is tested against, because QEMU has no Bluetooth adapter.
 *
 * It takes HCI packets as the stack sends them and gives back the packets
 * a controller would, in the ring's own form - a type byte first
 * (`hciring.h`). The USB driver puts it where an adapter would be under
 * `opt/kosmos/bluetooth=simulated`, so the stack, the rings and the pulses
 * are the real ones, and only the radio is pretended.
 *
 * It answers the controller's setup: Reset, the version, the address,
 * the features and the buffers, the event mask and the inquiry mode, and
 * Command Complete with Unknown HCI Command for anything else, as a real
 * controller does.
 *
 * **And it has neighbours** (B4), made up, each for a path the stack must
 * take: a Logitech K380 keyboard on Classic that names itself in its
 * extended inquiry response; a mouse on Low Energy that advertises HID and
 * its appearance; and headphones on Classic that say no name until a
 * Remote Name Request asks. An inquiry finds the two Classic ones, a scan
 * the mouse. Pairing and connecting to them come at B5 and B6.
 *
 * No hardware and no system calls: `tools/test_btdecode.c` holds it on the
 * Mac, against the same decoders the stack reads it with.
 */

#include <stdbool.h>
#include <stdint.h>

/* What it says it is: Bluetooth 5.1, company 0xFFFF - which Assigned
 * Numbers keeps for testing, so nothing real is ever taken for it - and a
 * locally administered address that reads "KOSMOS" in its last five. */
#define BTSIM_VERSION       10u
#define BTSIM_COMPANY       0xFFFFu
#define BTSIM_ACL_LENGTH    1021u
#define BTSIM_ACL_PACKETS   8u

/* Packets waiting to go up: a whole setup's answers, a scan's results and
 * more. */
#define BTSIM_QUEUE         24u
#define BTSIM_PACKET_MAX    260u

struct btsim {
    uint8_t  queue[BTSIM_QUEUE][BTSIM_PACKET_MAX];
    uint16_t length[BTSIM_QUEUE];
    unsigned head;                  /* the next to give */
    unsigned count;                 /* how many are waiting */
    unsigned dropped;               /* answers there was no room for */
    unsigned commands;              /* commands taken, since its start */
    bool     reset;                 /* a Reset has been taken */
    bool     scanning;              /* LE scanning is on */
    unsigned inquiries;             /* inquiries started */
    unsigned names_asked;           /* Remote Name Requests taken */
};

void btsim_start(struct btsim *s);

/* One packet from the stack, its type byte first. Anything that is not a
 * whole command is ignored, as a controller drops a malformed packet. */
void btsim_take(struct btsim *s, const uint8_t *packet, unsigned length);

/* The next packet for the stack into `out`, its type byte first; its
 * length, or 0 when there is none. `room` at least `BTSIM_PACKET_MAX`. */
unsigned btsim_next(struct btsim *s, uint8_t *out, unsigned room);

#endif
