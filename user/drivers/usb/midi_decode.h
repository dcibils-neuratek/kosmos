/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DRIVERS_USB_MIDI_DECODE_H
#define KOSMOS_DRIVERS_USB_MIDI_DECODE_H

/*
 * **A USB MIDI device, as its bytes describe it** (`roadmap.md` 6zg,
 * `usb.md` §12): which interface is its MIDIStreaming one, the bulk
 * endpoints it talks on, which of its ports each *cable* is and what the
 * port is called - and the four-byte event packets the notes arrive in and
 * leave in.
 *
 * Its own file with no hardware and no system calls in it, for the reason
 * `usb_decode.c` and `uvc_decode.c` are: the bytes are the device's to
 * choose, and QEMU has no MIDI device of its own. So `tools/test_mididecode.c`
 * holds this to Novation's Launchpad MK2 as Linux printed it, to the
 * Launchkey Mini MK3's two ports, and to the malformed bytes no real device
 * sends.
 *
 * The specification is Universal Serial Bus Device Class Definition for
 * MIDI Devices, 1.0: 6.1 and 6.2 for the descriptors, 4 for the packets.
 *
 * **A port is a cable.** A device with more than one port - the Launchkey
 * has its keys on one and its DAW controls on the other - carries them all
 * on the same two endpoints, and says which is which by the cable number in
 * each packet's first nibble. Which cable is which port is the order of the
 * jacks an endpoint's class descriptor lists: the first is cable 0.
 */

#include <stdbool.h>
#include <stdint.h>

#define MIDI_CABLES 16

/* One port, in one direction. */
struct midi_port {
    uint8_t cable;              /* 0 to 15: what a packet's first nibble says */
    uint8_t jack;               /* bJackID of its embedded jack */
    uint8_t name;               /* iJack: a string descriptor's index, or 0 */
};

struct usb_midi {
    bool     ok;                /* a MIDIStreaming interface, and an endpoint */
    uint8_t  configuration;     /* bConfigurationValue */
    uint8_t  interface;         /* the MIDIStreaming interface's number */
    uint8_t  alternate;         /* and its alternate setting */
    uint8_t  name;              /* its iInterface, or 0 */

    uint8_t  in_endpoint;       /* bulk IN, 1 to 15 - the device's notes - or 0 */
    uint16_t in_packet;         /* its wMaxPacketSize */
    uint8_t  out_endpoint;      /* bulk OUT, to the device, or 0 */
    uint16_t out_packet;

    unsigned nin;               /* ports the device sends on: `in` */
    struct midi_port in[MIDI_CABLES];
    unsigned nout;              /* ports it listens on: `out` */
    struct midi_port out[MIDI_CABLES];

    bool     malformed;         /* a length that walks off the end, or zero */
};

/*
 * `bytes` is what GET_DESCRIPTOR returned for the configuration and
 * `length` how many arrived. The first MIDIStreaming interface setting with
 * a bulk endpoint is the one taken. Nothing outside the bytes is read, and
 * a descriptor whose length is zero or runs past the end stops the walk
 * with `malformed` set and what came before it kept.
 *
 * A device that lists no jacks on an endpoint - the specification says it
 * must, and a device is free not to - gets one port on it, cable 0.
 */
void midi_decode_config(const uint8_t *bytes, unsigned length,
                        struct usb_midi *out);

/*
 * **One event**, out of a four-byte packet: its cable, and the MIDI bytes
 * it carries - one to three of them, as the packet's Code Index Number
 * says (MIDI 1.0 USB 4, table 4-1). Part of a System Exclusive message
 * arrives as its own event, three bytes at a time, with `sysex` set.
 */
struct midi_event {
    uint8_t cable;
    uint8_t length;             /* how many of `bytes` are the message's: 1 to 3 */
    bool    sysex;              /* a piece of a System Exclusive message */
    uint8_t bytes[3];
};

/*
 * False for a packet that carries nothing: a Code Index Number the
 * specification reserves (0 and 1) - which is also what the zeros padding a
 * short transfer out to its length look like.
 */
bool midi_decode_packet(const uint8_t packet[4], struct midi_event *out);

/*
 * **The other way**: one MIDI message - its status byte and what follows -
 * into packets for `cable`, at `out`, which has room for `room` of them.
 * Answers how many packets it wrote, or 0 for a message that is not one:
 * a data byte where the status should be, or too few bytes after it. A
 * System Exclusive message, F0 to F7, goes three bytes a packet.
 */
unsigned midi_encode(uint8_t cable, const uint8_t *message, unsigned length,
                     uint8_t (*out)[4], unsigned room);

/*
 * How many bytes a message that starts with `status` is, status included:
 * 3 for a note or a controller, 2 for a program change, 1 for the clock -
 * and 0 for System Exclusive, whose length is wherever its F7 is, or for a
 * byte that is not a status at all.
 */
unsigned midi_message_length(uint8_t status);

#endif /* KOSMOS_DRIVERS_USB_MIDI_DECODE_H */
