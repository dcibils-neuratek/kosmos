/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A USB MIDI device's bytes, decoded on the host (`roadmap.md` 6zg).
 *
 * QEMU has no MIDI device, so what `midi_decode_config` reads in a machine
 * comes from a real one. Two here:
 *
 *   - **Novation's Launchpad MK2** (1235:0069), rebuilt field for field from
 *     the `lsusb -D` a Linux machine printed of it (espressif/esp-idf issue
 *     11616): one port each way, bulk endpoints of 64 bytes - and no
 *     external jacks at all, which the specification asks for and Novation
 *     leaves out, so a parser that insisted on them would not find it;
 *   - **the Launchkey Mini MK3** (1235:0102), Diego's: its keys on one port
 *     and its DAW controls on the other, both on the same two endpoints.
 *     Composed from the specification with the names its ports go by, until
 *     its own bytes are read from it; the shape is what matters here - two
 *     jacks listed on each endpoint, two cables.
 *
 * And what no real device sends: every length of the Launchkey cut short on
 * the last byte before a page that is not mapped, a descriptor of length
 * zero, an audio device with no MIDI, a MIDIStreaming interface with no
 * endpoints, and a first setting without endpoints before one with them.
 * Then the packets both ways: notes, controllers, a bend, a program, the
 * clock, System Exclusive in pieces, padding, and messages that are not.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../user/drivers/usb/midi_decode.h"

static int checks;
static int fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  not ok: %s\n", what);
    }
}

/* The Launchpad MK2's configuration, as `lsusb -D` printed it. */
static const uint8_t MK2[] = {
    9, 0x02, 82, 0, 2, 1, 0, 0x80, 50,              /* configuration 1 */
    9, 0x04, 0, 0, 0, 0x01, 0x01, 0, 3,             /* AudioControl, iInterface 3 */
    9, 0x24, 0x01, 0x00, 0x01, 9, 0, 1, 1,          /* its header: interface 1 */
    9, 0x04, 1, 0, 2, 0x01, 0x03, 0, 3,             /* MIDIStreaming, 2 endpoints */
    7, 0x24, 0x01, 0x00, 0x01, 0x2E, 0x00,          /* MS header, 46 bytes */
    6, 0x24, 0x02, 0x01, 1, 0,                      /* IN jack 1, embedded */
    9, 0x24, 0x03, 0x01, 2, 1, 1, 1, 0,             /* OUT jack 2, from jack 1 */
    7, 0x05, 0x81, 0x02, 0x40, 0x00, 1,             /* EP 1 IN, bulk, 64 */
    5, 0x25, 0x01, 1, 2,                            /* carries jack 2 */
    7, 0x05, 0x02, 0x02, 0x40, 0x00, 1,             /* EP 2 OUT, bulk, 64 */
    5, 0x25, 0x01, 1, 1,                            /* carries jack 1 */
};

/*
 * The Launchkey Mini MK3's shape: two embedded jacks each way, named by
 * strings 4 ("MIDI") and 5 ("DAW"), the nine-byte endpoints of the audio
 * class, and each endpoint listing both jacks - cable 0 the keys, cable 1
 * the DAW port.
 */
static const uint8_t LAUNCHKEY[] = {
    9, 0x02, 103, 0, 2, 1, 0, 0x80, 50,
    9, 0x04, 0, 0, 0, 0x01, 0x01, 0, 0,
    9, 0x24, 0x01, 0x00, 0x01, 9, 0, 1, 1,
    9, 0x04, 1, 0, 2, 0x01, 0x03, 0, 2,             /* iInterface 2 */
    7, 0x24, 0x01, 0x00, 0x01, 0x43, 0x00,          /* 67 bytes */
    6, 0x24, 0x02, 0x01, 1, 4,                      /* IN jack 1, "MIDI" */
    6, 0x24, 0x02, 0x01, 2, 5,                      /* IN jack 2, "DAW" */
    9, 0x24, 0x03, 0x01, 3, 1, 1, 1, 4,             /* OUT jack 3, "MIDI" */
    9, 0x24, 0x03, 0x01, 4, 1, 2, 1, 5,             /* OUT jack 4, "DAW" */
    9, 0x05, 0x81, 0x02, 0x40, 0x00, 0, 0, 0,       /* EP 1 IN */
    6, 0x25, 0x01, 2, 3, 4,                         /* cables 0 and 1 */
    9, 0x05, 0x02, 0x02, 0x40, 0x00, 0, 0, 0,       /* EP 2 OUT */
    6, 0x25, 0x01, 2, 1, 2,
};

static void test_mk2(void)
{
    struct usb_midi m;

    check(sizeof MK2 == 82, "the MK2's bytes are the 82 its configuration says");
    midi_decode_config(MK2, sizeof MK2, &m);

    check(m.ok && !m.malformed, "the MK2 is a MIDI device");
    check(m.configuration == 1, "its configuration is 1");
    check(m.interface == 1 && m.alternate == 0, "its MIDI is interface 1, setting 0");
    check(m.name == 3, "named by string 3");
    check(m.in_endpoint == 1 && m.in_packet == 64, "it sends on endpoint 1, 64 bytes");
    check(m.out_endpoint == 2 && m.out_packet == 64, "it listens on endpoint 2, 64 bytes");
    check(m.nin == 1 && m.in[0].cable == 0 && m.in[0].jack == 2,
          "one port it sends on: cable 0, its OUT jack 2");
    check(m.nout == 1 && m.out[0].cable == 0 && m.out[0].jack == 1,
          "one port it listens on: cable 0, its IN jack 1");
}

static void test_launchkey(void)
{
    struct usb_midi m;

    check(sizeof LAUNCHKEY == 103, "the Launchkey's bytes are the 103 its configuration says");
    midi_decode_config(LAUNCHKEY, sizeof LAUNCHKEY, &m);

    check(m.ok && !m.malformed, "the Launchkey is a MIDI device");
    check(m.name == 2, "named by string 2");
    check(m.nin == 2 && m.nout == 2, "two ports each way");
    check(m.in[0].cable == 0 && m.in[0].jack == 3 && m.in[0].name == 4,
          "cable 0 in is its keys, jack 3, named by string 4");
    check(m.in[1].cable == 1 && m.in[1].jack == 4 && m.in[1].name == 5,
          "cable 1 in is its DAW port, jack 4, named by string 5");
    check(m.out[0].jack == 1 && m.out[0].name == 4 && m.out[1].jack == 2 && m.out[1].name == 5,
          "and the same two names out");
}

/* A copy of the first `n` bytes ending on the byte before an unmapped page. */
static uint8_t *guarded;
static size_t guarded_room;

static uint8_t *on_the_edge(const uint8_t *bytes, unsigned n)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);

    if (guarded == NULL) {
        guarded_room = page;
        guarded = mmap(NULL, guarded_room + page, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);

        if (guarded == MAP_FAILED
            || mprotect(guarded + guarded_room, page, PROT_NONE) != 0) {
            printf("not ok - no guard page to test against\n");
            exit(1);
        }
    }

    memcpy(guarded + guarded_room - n, bytes, n);
    return guarded + guarded_room - n;
}

static void test_cut_short(void)
{
    struct usb_midi m;
    int wrong = 0;

    for (unsigned n = 0; n <= sizeof LAUNCHKEY; n++) {
        unsigned at = 0;

        while (at < n) at += LAUNCHKEY[at];

        midi_decode_config(on_the_edge(LAUNCHKEY, n), n, &m);

        if ((at == n) == m.malformed) wrong++;
    }

    check(wrong == 0, "every length of the Launchkey cut short, read in bounds, "
                      "refused exactly where a descriptor is cut");

    {
        uint8_t zero[] = { 9, 0x02, 11, 0, 1, 1, 0, 0x80, 50, 0, 4 };

        midi_decode_config(zero, sizeof zero, &m);
        check(m.malformed && !m.ok, "a descriptor of length zero stops it");
    }
}

static void test_not_midi(void)
{
    struct usb_midi m;

    /* Audio streaming, subclass 2: sound, not MIDI. */
    const uint8_t audio[] = {
        9, 0x02, 25, 0, 1, 1, 0, 0x80, 50,
        9, 0x04, 1, 1, 1, 0x01, 0x02, 0, 0,
        7, 0x05, 0x81, 0x01, 0xC0, 0x00, 1,
    };

    midi_decode_config(audio, sizeof audio, &m);
    check(!m.ok, "a device that streams audio is not a MIDI one");

    /* A MIDIStreaming interface with nothing to talk on. */
    const uint8_t mute[] = {
        9, 0x02, 25, 0, 1, 1, 0, 0x80, 50,
        9, 0x04, 1, 0, 0, 0x01, 0x03, 0, 0,
        7, 0x24, 0x01, 0x00, 0x01, 0x07, 0x00,
    };

    midi_decode_config(mute, sizeof mute, &m);
    check(!m.ok, "a MIDIStreaming interface with no endpoint is refused");

    /* Setting 0 with none, setting 1 with both: setting 1 is the one. */
    const uint8_t two[] = {
        9, 0x02, 41, 0, 1, 1, 0, 0x80, 50,
        9, 0x04, 1, 0, 0, 0x01, 0x03, 0, 0,
        9, 0x04, 1, 1, 2, 0x01, 0x03, 0, 0,
        7, 0x05, 0x83, 0x02, 0x00, 0x02, 0,
        7, 0x05, 0x04, 0x02, 0x00, 0x02, 0,
    };

    midi_decode_config(two, sizeof two, &m);
    check(m.ok && m.alternate == 1 && m.in_endpoint == 3 && m.out_endpoint == 4,
          "the setting with endpoints is taken over the one without");
    check(m.in_packet == 512 && m.nin == 1 && m.nout == 1,
          "a high-speed packet of 512, and cable 0 where no jacks are listed");
}

static int packet_is(const uint8_t p[4], uint8_t cable, uint8_t len, int sysex,
                     uint8_t b0, uint8_t b1, uint8_t b2)
{
    struct midi_event e;

    if (!midi_decode_packet(p, &e)) return 0;

    return e.cable == cable && e.length == len && e.sysex == (sysex != 0)
        && e.bytes[0] == b0 && e.bytes[1] == b1 && e.bytes[2] == b2;
}

static void test_packets(void)
{
    struct midi_event e;
    const uint8_t on[4] = { 0x09, 0x90, 60, 100 };
    const uint8_t off[4] = { 0x18, 0x80, 60, 0 };
    const uint8_t cc[4] = { 0x0B, 0xB0, 21, 64 };
    const uint8_t bend[4] = { 0x0E, 0xE0, 0x00, 0x40 };
    const uint8_t program[4] = { 0x0C, 0xC0, 5, 99 };
    const uint8_t clock[4] = { 0x0F, 0xF8, 7, 7 };
    const uint8_t sx1[4] = { 0x04, 0xF0, 0x7E, 0x7F };
    const uint8_t sx2[4] = { 0x07, 0x06, 0x01, 0xF7 };
    const uint8_t tune[4] = { 0x05, 0xF6, 0, 0 };
    const uint8_t end[4] = { 0x05, 0xF7, 0, 0 };
    const uint8_t pad[4] = { 0, 0, 0, 0 };
    const uint8_t reserved[4] = { 0x01, 0x90, 60, 100 };

    check(packet_is(on, 0, 3, 0, 0x90, 60, 100), "a note on, cable 0");
    check(packet_is(off, 1, 3, 0, 0x80, 60, 0), "a note off, cable 1");
    check(packet_is(cc, 0, 3, 0, 0xB0, 21, 64), "a controller");
    check(packet_is(bend, 0, 3, 0, 0xE0, 0x00, 0x40), "a pitch bend");
    check(packet_is(program, 0, 2, 0, 0xC0, 5, 0), "a program change is two bytes, the third not read");
    check(packet_is(clock, 0, 1, 0, 0xF8, 0, 0), "the clock is one byte");
    check(packet_is(sx1, 0, 3, 1, 0xF0, 0x7E, 0x7F) && packet_is(sx2, 0, 3, 1, 0x06, 0x01, 0xF7),
          "System Exclusive, begun and ended");
    check(packet_is(tune, 0, 1, 0, 0xF6, 0, 0), "a tune request is System Common, not System Exclusive");
    check(packet_is(end, 0, 1, 1, 0xF7, 0, 0), "a lone F7 ends System Exclusive");
    check(!midi_decode_packet(pad, &e), "padding carries nothing");
    check(!midi_decode_packet(reserved, &e), "a reserved code carries nothing");
}

static int encodes(uint8_t cable, const uint8_t *msg, unsigned len, unsigned packets,
                   const uint8_t (*want)[4])
{
    uint8_t out[8][4];
    unsigned n = midi_encode(cable, msg, len, out, 8);

    if (n != packets) return 0;

    return packets == 0 || memcmp(out, want, packets * 4) == 0;
}

static void test_encode(void)
{
    const uint8_t on[] = { 0x90, 0x3C, 0x64 };
    const uint8_t daw[] = { 0x9F, 0x0C, 0x7F };
    const uint8_t program[] = { 0xC0, 0x05 };
    const uint8_t clock[] = { 0xF8 };
    const uint8_t song[] = { 0xF2, 0x10, 0x20 };
    const uint8_t sx6[] = { 0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7 };
    const uint8_t sx4[] = { 0xF0, 0x01, 0x02, 0xF7 };
    const uint8_t sx5[] = { 0xF0, 0x01, 0x02, 0x03, 0xF7 };
    const uint8_t unended[] = { 0xF0, 0x01, 0x02 };
    const uint8_t data[] = { 0x3C, 0x64 };
    const uint8_t short_[] = { 0x90, 0x3C };
    const uint8_t high[] = { 0x90, 0x3C, 0x90 };

    const uint8_t w_on[1][4] = { { 0x19, 0x90, 0x3C, 0x64 } };
    const uint8_t w_daw[1][4] = { { 0x19, 0x9F, 0x0C, 0x7F } };
    const uint8_t w_program[1][4] = { { 0x0C, 0xC0, 0x05, 0 } };
    const uint8_t w_clock[1][4] = { { 0x0F, 0xF8, 0, 0 } };
    const uint8_t w_song[1][4] = { { 0x03, 0xF2, 0x10, 0x20 } };
    const uint8_t w_sx6[2][4] = { { 0x04, 0xF0, 0x7E, 0x7F }, { 0x07, 0x06, 0x01, 0xF7 } };
    const uint8_t w_sx4[2][4] = { { 0x04, 0xF0, 0x01, 0x02 }, { 0x05, 0xF7, 0, 0 } };
    const uint8_t w_sx5[2][4] = { { 0x04, 0xF0, 0x01, 0x02 }, { 0x06, 0x03, 0xF7, 0 } };

    check(encodes(1, on, 3, 1, w_on), "a note on, onto cable 1");
    check(encodes(1, daw, 3, 1, w_daw), "the Launchkey's DAW mode, asked for on its DAW port");
    check(encodes(0, program, 2, 1, w_program), "a program change, two bytes");
    check(encodes(0, clock, 1, 1, w_clock), "the clock, one byte");
    check(encodes(0, song, 3, 1, w_song), "a song position, System Common of three");
    check(encodes(0, sx6, 6, 2, w_sx6), "System Exclusive of six bytes in two packets");
    check(encodes(0, sx4, 4, 2, w_sx4), "of four, ending on a packet of one");
    check(encodes(0, sx5, 5, 2, w_sx5), "of five, ending on a packet of two");
    check(encodes(0, unended, 3, 0, NULL), "System Exclusive with no end is refused");
    check(encodes(0, data, 2, 0, NULL), "a data byte where the status goes is refused");
    check(encodes(0, short_, 2, 0, NULL), "a note with its velocity missing is refused");
    check(encodes(0, high, 3, 0, NULL), "a status byte where a data byte goes is refused");

    {
        uint8_t out[1][4];

        check(midi_encode(0, sx6, 6, out, 1) == 0, "and a message that does not fit its room");
    }

    /* Every channel message there and back, on every cable. */
    int round = 1;

    for (unsigned status = 0x80; status < 0xF0; status += 0x10) {
        for (unsigned cable = 0; cable < 16; cable++) {
            uint8_t msg[3] = { (uint8_t)(status | (cable & 0xF)), 0x12, 0x34 };
            uint8_t out[1][4];
            struct midi_event e;
            unsigned n = midi_message_length(msg[0]);

            if (midi_encode((uint8_t)cable, msg, n, out, 1) != 1
                || !midi_decode_packet(out[0], &e)
                || e.cable != cable || e.length != n || e.bytes[0] != msg[0]
                || e.bytes[1] != 0x12 || (n == 3 && e.bytes[2] != 0x34)) {
                round = 0;
            }
        }
    }

    check(round, "every channel message on every cable, there and back");
}

int main(void)
{
    test_mk2();
    test_launchkey();
    test_cut_short();
    test_not_midi();
    test_packets();
    test_encode();

    if (fails) {
        printf("FAIL: %d of %d checks on USB MIDI decoding\n", fails, checks + fails);
        return 1;
    }

    printf("PASS: %d checks on USB MIDI decoding (Novation's Launchpad MK2 from "
           "its lsusb, the Launchkey Mini MK3's two ports, every length cut short, "
           "devices that are not, and packets both ways)\n", checks);
    return 0;
}
