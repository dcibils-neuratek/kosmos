/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A USB Audio Class device's bytes, decoded on the host (`usbaudio.md` U1).
 *
 * The fixtures:
 *
 *   - QEMU's own speaker, `-device usb-audio`, byte for byte as
 *     `hw/usb/dev-audio.c` in QEMU 11.1.1 lays it out: UAC1, one setting,
 *     48 kHz, 16 bits, two channels, synchronous - the device the gate plays
 *     through. And its `multi=on` variant: stereo, 5.1 and 7.1;
 *   - a UAC2 DAC at high speed laid out as the specification lays one out:
 *     asynchronous, a feedback endpoint, 16, 24 and 32 bits, its rates from
 *     its clock source's GET RANGE;
 *   - a UAC1 headset: a speaker that is asynchronous by bSynchAddress, as
 *     USB 1.1 devices say it, a microphone, a setting in AC-3 that is not
 *     read, and a HID interface for its buttons;
 *   - a MIDI keyboard, which has an AudioControl interface and no sound;
 *   - for the speaker's own volume (U10g): QEMU's, a master mute and a
 *     volume on each channel, with the range `hw/usb/dev-audio.c` answers;
 *     the M60's and the DAC's, on the master; a UAC1 speaker whose volume
 *     is on its channels only, behind a unit with a mute, after a
 *     microphone's unit; a UAC2 RANGE of two subranges; one whose controls
 *     are read-only; and the headset, with none - every request's bytes,
 *     and the master's levels mapped onto each;
 *   - every fixture cut short at every length, each copy ending on the last
 *     byte before a page that is not mapped, so a read past the end is a
 *     crash rather than a pass.
 *
 * Its control: `UACTEST_BROKEN=1` corrupts one field in each fixture before
 * decoding, and the run must fail.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../user/drivers/usb/uac_decode.h"
#include "../user/drivers/usb/uac_stream.h"

static int checks;
static int fails;
static int broken;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  not ok: %s\n", what);
    }
}

#define U16(x) ((x) & 0xff), (((x) >> 8) & 0xff)
#define U24(x) U16(x), (((x) >> 16) & 0xff)
#define U32(x) U24(x), (((x) >> 24) & 0xff)

/* ---- QEMU's speaker, hw/usb/dev-audio.c's desc_iface, as usb_desc_config
 * writes it: an endpoint with is_audio set is nine bytes, its bRefresh and
 * bSynchAddress zero. wTotalLength is 0x71: 113 bytes. */
static uint8_t QEMU[] = {
    9, 0x02, U16(113), 2, 1, 0, 0xC0, 0x32,
    /* interface 0: AudioControl */
    9, 0x04, 0, 0, 0, 0x01, 0x01, 0x00, 0,
    9, 0x24, 0x01, U16(0x0100), U16(0x2b), 1, 1,
    12, 0x24, 0x02, 1, U16(0x0101), 0, 2, U16(0x0003), 0, 0,
    13, 0x24, 0x06, 2, 1, 2, U16(0x0001), U16(0x0002), U16(0x0002), 0,
    9, 0x24, 0x03, 3, U16(0x0301), 0, 2, 0,
    /* interface 1, setting 0: no endpoint */
    9, 0x04, 1, 0, 0, 0x01, 0x02, 0x00, 0,
    /* interface 1, setting 1: stereo */
    9, 0x04, 1, 1, 1, 0x01, 0x02, 0x00, 0,
    7, 0x24, 0x01, 1, 0, U16(0x0001),
    11, 0x24, 0x02, 1, 2, 2, 16, 1, U24(48000),
    9, 0x05, 0x01, 0x0d, U16(192), 1, 0, 0,
    7, 0x25, 0x01, 0, 0, U16(0),
};

/* The same with `multi=on`: settings 2 and 3, 5.1 and 7.1, after stereo. */
#define QEMU_SETTING(alt, ch) \
    9, 0x04, 1, alt, 1, 0x01, 0x02, 0x00, 0, \
    7, 0x24, 0x01, 1, 0, U16(0x0001), \
    11, 0x24, 0x02, 1, ch, 2, 16, 1, U24(48000), \
    9, 0x05, 0x01, 0x0d, U16(96 * ch), 1, 0, 0, \
    7, 0x25, 0x01, 0, 0, U16(0)

static uint8_t QEMU_MULTI[] = {
    9, 0x02, U16(0), 2, 1, 0, 0xC0, 0x32,
    9, 0x04, 0, 0, 0, 0x01, 0x01, 0x00, 0,
    9, 0x24, 0x01, U16(0x0100), U16(0x2b), 1, 1,
    12, 0x24, 0x02, 1, U16(0x0101), 0, 8, U16(0x063f), 0, 0,
    9, 0x24, 0x03, 3, U16(0x0301), 0, 1, 0,
    9, 0x04, 1, 0, 0, 0x01, 0x02, 0x00, 0,
    QEMU_SETTING(1, 2),
    QEMU_SETTING(2, 6),
    QEMU_SETTING(3, 8),
};

/* ---- A UAC2 DAC, high speed (UAC2 4.7-4.10, FMT20 2.3.1.6). Interface 0
 * AudioControl with an Interface Association before it; interface 1
 * streams out, asynchronous, with a feedback endpoint in every setting. */
#define DAC_SETTING(alt, slot, bits, packet) \
    9, 0x04, 1, alt, 2, 0x01, 0x02, 0x20, 0, \
    16, 0x24, 0x01, 0x10, 0x00, 0x01, U32(0x00000001), 2, U32(0x3), 0, \
    6, 0x24, 0x02, 0x01, slot, bits, \
    7, 0x05, 0x01, 0x05, U16(packet), 1, \
    8, 0x25, 0x01, 0, 0, 0, U16(0), \
    7, 0x05, 0x81, 0x11, U16(4), 4

static uint8_t DAC[] = {
    9, 0x02, U16(0), 2, 1, 0, 0x80, 0xFA,
    8, 0x0B, 0, 2, 0x01, 0x00, 0x20, 0,
    9, 0x04, 0, 0, 0, 0x01, 0x01, 0x20, 0,
    9, 0x24, 0x01, U16(0x0200), 0x08, U16(64), 0,
    8, 0x24, 0x0A, 0x29, 0x03, 0x07, 0, 0,
    17, 0x24, 0x02, 0x02, U16(0x0101), 0, 0x29, 2, U32(0x3), 0, U16(0), 0,
    18, 0x24, 0x06, 0x0A, 0x02, U32(0xF), U32(0xC), U32(0xC), 0,
    12, 0x24, 0x03, 0x14, U16(0x0302), 0, 0x0A, 0x29, U16(0), 0,
    9, 0x04, 1, 0, 0, 0x01, 0x02, 0x20, 0,
    DAC_SETTING(1, 2, 16, 104),
    DAC_SETTING(2, 3, 24, 156),
    DAC_SETTING(3, 4, 32, 208),
};

/* ---- A UAC1 headset: a speaker asynchronous by bSynchAddress, a
 * microphone, an AC-3 setting nothing here reads, and its buttons. */
static uint8_t HEADSET[] = {
    9, 0x02, U16(0), 4, 1, 0, 0x80, 0x32,
    /* interface 0: AudioControl, both paths */
    9, 0x04, 0, 0, 0, 0x01, 0x01, 0x00, 0,
    10, 0x24, 0x01, U16(0x0100), U16(0), 2, 1, 2,
    12, 0x24, 0x02, 1, U16(0x0101), 0, 2, U16(0x3), 0, 0,
    9, 0x24, 0x03, 3, U16(0x0402), 0, 1, 0,
    12, 0x24, 0x02, 4, U16(0x0201), 0, 1, U16(0), 0, 0,
    9, 0x24, 0x03, 6, U16(0x0101), 0, 4, 0,
    /* interface 1: the speaker. Setting 1: 16 bits, 44.1 or 48 kHz, its
     * OUT naming 0x82 as its feedback; setting 2: AC-3, not read */
    9, 0x04, 1, 0, 0, 0x01, 0x02, 0x00, 0,
    9, 0x04, 1, 1, 2, 0x01, 0x02, 0x00, 0,
    7, 0x24, 0x01, 1, 1, U16(0x0001),
    14, 0x24, 0x02, 1, 2, 2, 16, 2, U24(44100), U24(48000),
    9, 0x05, 0x01, 0x05, U16(196), 1, 0, 0x82,
    7, 0x25, 0x01, 0x01, 0, U16(0),
    9, 0x05, 0x82, 0x01, U16(3), 1, 5, 0,
    9, 0x04, 1, 2, 1, 0x01, 0x02, 0x00, 0,
    7, 0x24, 0x01, 1, 1, U16(0x2001),
    9, 0x05, 0x01, 0x05, U16(196), 1, 0, 0,
    /* interface 2: the microphone, mono, 8 to 48 kHz as a range */
    9, 0x04, 2, 0, 0, 0x01, 0x02, 0x00, 0,
    9, 0x04, 2, 1, 1, 0x01, 0x02, 0x00, 0,
    7, 0x24, 0x01, 6, 1, U16(0x0001),
    14, 0x24, 0x02, 1, 1, 2, 16, 0, U24(8000), U24(48000),
    9, 0x05, 0x83, 0x0D, U16(96), 1, 0, 0,
    7, 0x25, 0x01, 0x01, 0, U16(0),
    /* interface 3: HID, its volume and mute */
    9, 0x04, 3, 0, 1, 0x03, 0x00, 0x00, 0,
    9, 0x21, U16(0x0111), 0, 1, 0x22, U16(33),
    7, 0x05, 0x84, 0x03, U16(4), 10,
};

/* ---- A MIDI keyboard: AudioControl, then MIDIStreaming (subclass 3). */
/*
 * **Diego's EDIFIER M60** (2d99:a094), byte for byte as the M700's xHCI
 * driver said it on 22 November 2026 (`testing.md` 18.998): UAC1 at full
 * speed, a speaker on interface 1 with two settings - 16 and 24 bits, both
 * at 48 or 96 kHz, synchronous on endpoint 3 OUT - and its volume buttons
 * on a HID interface 2 beside it.
 */
static uint8_t M60[] = {
    0x09, 0x02, 0xbb, 0x00, 0x03, 0x01, 0x00, 0xa0, 0x32, 0x09, 0x04, 0x00,
    0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x09, 0x24, 0x01, 0x00, 0x01, 0x2b,
    0x00, 0x01, 0x01, 0x0c, 0x24, 0x02, 0x04, 0x01, 0x01, 0x00, 0x02, 0x03,
    0x00, 0x00, 0x00, 0x0d, 0x24, 0x06, 0x05, 0x04, 0x02, 0x03, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x09, 0x24, 0x03, 0x06, 0x01, 0x03, 0x00, 0x05,
    0x00, 0x09, 0x04, 0x01, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x09, 0x04,
    0x01, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00, 0x07, 0x24, 0x01, 0x04, 0x00,
    0x01, 0x00, 0x0e, 0x24, 0x02, 0x01, 0x02, 0x02, 0x10, 0x02, 0x80, 0xbb,
    0x00, 0x00, 0x77, 0x01, 0x09, 0x05, 0x03, 0x0d, 0x80, 0x01, 0x01, 0x00,
    0x00, 0x07, 0x25, 0x01, 0x01, 0x00, 0x00, 0x00, 0x09, 0x04, 0x01, 0x02,
    0x01, 0x01, 0x02, 0x00, 0x00, 0x07, 0x24, 0x01, 0x04, 0x00, 0x01, 0x00,
    0x0e, 0x24, 0x02, 0x01, 0x02, 0x03, 0x18, 0x02, 0x80, 0xbb, 0x00, 0x00,
    0x77, 0x01, 0x09, 0x05, 0x03, 0x0d, 0x40, 0x02, 0x01, 0x00, 0x00, 0x07,
    0x25, 0x01, 0x01, 0x00, 0x00, 0x00, 0x09, 0x04, 0x02, 0x00, 0x01, 0x03,
    0x00, 0x00, 0x00, 0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x51, 0x00,
    0x07, 0x05, 0x84, 0x03, 0x03, 0x00, 0x04,
};

static uint8_t MIDI[] = {
    9, 0x02, U16(0), 2, 1, 0, 0x80, 0x32,
    9, 0x04, 0, 0, 0, 0x01, 0x01, 0x00, 0,
    9, 0x24, 0x01, U16(0x0100), U16(9), 1, 1,
    9, 0x04, 1, 0, 2, 0x01, 0x03, 0x00, 0,
    7, 0x24, 0x01, U16(0x0100), U16(37),
    9, 0x05, 0x01, 0x02, U16(64), 0, 0, 0,
    5, 0x25, 0x01, 1, 1,
    9, 0x05, 0x81, 0x02, U16(64), 0, 0, 0,
    5, 0x25, 0x01, 1, 3,
};

/* ---- A UAC1 speaker whose volume is on its two channels only (U10g). Its
 * microphone's feature unit, 5, comes first, so the first feature unit is
 * not the speaker's; the speaker's path is output terminal 3 <- unit 8, a
 * master mute only, <- unit 7, a volume on channel 1 and on channel 2 and
 * none on the master, <- input terminal 1 - and terminal 3 is described
 * before unit 7, so the path is found whatever the order. bControlSize 1:
 * bLength 7 + (ch + 1) x 1 (Audio 1.0 Table 4-7). */
static uint8_t PERCH[] = {
    9, 0x02, U16(0), 2, 1, 0, 0x80, 0x32,
    9, 0x04, 0, 0, 0, 0x01, 0x01, 0x00, 0,
    9, 0x24, 0x01, U16(0x0100), U16(0), 1, 1,
    12, 0x24, 0x02, 1, U16(0x0101), 0, 2, U16(0x3), 0, 0,
    12, 0x24, 0x02, 4, U16(0x0201), 0, 1, U16(0), 0, 0,
    9, 0x24, 0x06, 5, 4, 1, 0x03, 0x00, 0,
    9, 0x24, 0x03, 6, U16(0x0101), 0, 5, 0,
    9, 0x24, 0x03, 3, U16(0x0301), 0, 8, 0,
    10, 0x24, 0x06, 7, 1, 1, 0x00, 0x02, 0x02, 0,
    10, 0x24, 0x06, 8, 7, 1, 0x01, 0x00, 0x00, 0,
    9, 0x04, 1, 0, 0, 0x01, 0x02, 0x00, 0,
    9, 0x04, 1, 1, 1, 0x01, 0x02, 0x00, 0,
    7, 0x24, 0x01, 1, 0, U16(0x0001),
    11, 0x24, 0x02, 1, 2, 2, 16, 1, U24(48000),
    9, 0x05, 0x01, 0x09, U16(192), 1, 0, 0,
    7, 0x25, 0x01, 0, 0, U16(0),
};

static void total(uint8_t *bytes, unsigned n)
{
    bytes[2] = (uint8_t)n;
    bytes[3] = (uint8_t)(n >> 8);
}

/* Under the control, one field the decoder depends on is made wrong. */
static void corrupt(uint8_t *bytes, unsigned n, unsigned at, uint8_t value)
{
    if (broken && at < n) {
        bytes[at] = value;
    }
}

/* Where in `bytes` the `nth` descriptor (from 1) of `type` - and
 * `subtype`, when not 0 - starts, walking from the first; or the length. */
static unsigned nth(const uint8_t *bytes, unsigned n, uint8_t type,
                    uint8_t subtype, unsigned which)
{
    unsigned at = 0;

    while (at + 2 < n && bytes[at] != 0) {
        if (bytes[at + 1] == type
            && (subtype == 0 || bytes[at + 2] == subtype) && --which == 0) {
            return at;
        }
        at += bytes[at];
    }

    return n;
}

static void test_qemu(void)
{
    struct uac_device d;
    const struct uac_alt *a;
    uint32_t rate = 1;
    int i;

    check(sizeof QEMU == 113, "QEMU's speaker is the 113 bytes it says");
    /* The control: its endpoint made bulk. */
    corrupt(QEMU, sizeof QEMU, nth(QEMU, sizeof QEMU, 0x05, 0, 1) + 3, 0x02);

    uac_decode_config(QEMU, sizeof QEMU, &d);
    a = &d.alts[0];

    check(d.ok && !d.malformed, "QEMU's speaker is audio, well formed");
    check(d.configuration == 1 && d.control == 0, "configuration 1, control 0");
    check(d.version == 1 && d.adc == 0x0100, "UAC1, bcdADC 1.00");
    check(d.feature == 2 && d.clock == 0, "feature unit 2, no clock source");
    check(d.output_type == 0x0301 && d.input_type == 0,
          "it plays into a speaker and records from nothing");
    check(d.plays && !d.records, "it plays and does not record");
    check(strcmp(uac_kind(&d), "speaker") == 0, "it is a speaker");
    check(d.nalts == 1 && d.skipped == 0 && d.alts_dropped == 0,
          "one setting carries sound; the empty setting 0 is not one");
    check(a->interface == 1 && a->alternate == 1, "interface 1, setting 1");
    check(!a->in && a->endpoint == 1, "OUT, endpoint 1");
    check(a->sync == UAC_SYNC_SYNC && a->feedback == 0,
          "synchronous, with no feedback endpoint");
    check(a->packet == 192 && a->extra == 0 && a->interval == 1,
          "192 bytes a frame, every frame");
    check(a->terminal == 1, "it streams to terminal 1");
    check(a->channels == 2 && a->subframe == 2 && a->bits == 16,
          "two channels of 16 bits in 2 bytes");
    check(!a->continuous && a->nrates == 1 && a->rates[0] == 48000,
          "48 kHz, and only that");
    check(uac_takes(a, 48000) && !uac_takes(a, 44100), "takes 48 kHz, not 44.1");
    check(!a->sets_rate, "and its rate is not to be set: it has one");

    i = uac_pick(&d, false, 44100, &rate);
    check(i == 0 && rate == 48000,
          "for a 44.1 kHz mix it plays setting 1 at 48 kHz, the nearest above");
    check(uac_pick(&d, true, 44100, &rate) == -1 && rate == 0,
          "and there is nothing to record on");
}

static void test_qemu_multi(void)
{
    struct uac_device d;
    uint32_t rate = 0;
    int i;

    total(QEMU_MULTI, sizeof QEMU_MULTI);
    /* The control: the stereo setting's channels made one. Its Format
     * Type is the second subtype 2, after the input terminal's. */
    corrupt(QEMU_MULTI, sizeof QEMU_MULTI,
            nth(QEMU_MULTI, sizeof QEMU_MULTI, 0x24, 0x02, 2) + 4, 1);

    uac_decode_config(QEMU_MULTI, sizeof QEMU_MULTI, &d);

    check(d.ok && d.nalts == 3, "QEMU's multi-channel speaker: three settings");
    check(d.alts[0].channels == 2 && d.alts[1].channels == 6
          && d.alts[2].channels == 8, "stereo, 5.1 and 7.1");
    check(d.alts[1].packet == 576 && d.alts[2].packet == 768,
          "96 bytes a channel a frame");

    i = uac_pick(&d, false, 44100, &rate);
    check(i == 0 && d.alts[i].alternate == 1 && rate == 48000,
          "a stereo mix plays on the stereo setting");
}

static void test_dac(void)
{
    struct uac_device d;
    struct uac_alt *a;
    uint32_t rate = 1;
    int i;
    /* GET RANGE as such a DAC answers: six rates, one subrange each. */
    static const uint8_t ranges[] = {
        U16(6),
        U32(44100), U32(44100), U32(0),   U32(48000), U32(48000), U32(0),
        U32(88200), U32(88200), U32(0),   U32(96000), U32(96000), U32(0),
        U32(176400), U32(176400), U32(0), U32(192000), U32(192000), U32(0),
    };
    /* And as one with a single wide subrange, every hertz. */
    static const uint8_t wide[] = { U16(1), U32(8000), U32(96000), U32(1) };

    total(DAC, sizeof DAC);
    /* The control: the second setting's protocol made UAC1's, so its
     * AS_GENERAL is read in the wrong layout and it is not PCM. */
    corrupt(DAC, sizeof DAC, nth(DAC, sizeof DAC, 0x04, 0, 4) + 7, 0x00);

    uac_decode_config(DAC, sizeof DAC, &d);

    check(d.ok && !d.malformed, "the DAC is audio, well formed");
    check(d.version == 2 && d.adc == 0x0200, "UAC2, bcdADC 2.00");
    check(d.clock == 0x29 && d.feature == 0x0A, "clock source 0x29, feature unit 0x0A");
    check(d.output_type == 0x0302, "it plays into headphones");
    check(strcmp(uac_kind(&d), "headphones") == 0, "it is headphones");
    check(d.nalts == 3 && d.skipped == 0, "three settings carry sound");
    check(d.alts[0].bits == 16 && d.alts[1].bits == 24 && d.alts[2].bits == 32
          && d.alts[1].subframe == 3, "16, 24 and 32 bits; 24 in three bytes");
    check(d.alts[0].channels == 2, "two channels, from AS_GENERAL in UAC2");

    a = &d.alts[1];
    check(a->sync == UAC_SYNC_ASYNC && a->endpoint == 1 && !a->in,
          "asynchronous, OUT on endpoint 1");
    check(a->feedback == 1 && a->feedback_packet == 4
          && a->feedback_interval == 4,
          "its feedback on endpoint 1 IN, 4 bytes every 8 microframes");
    check(a->packet == 156 && a->interval == 1, "156 bytes a microframe");
    check(a->nrates == 0, "its rates are not in its descriptors");

    i = uac_pick(&d, false, 44100, &rate);
    check(i == 0 && rate == 0,
          "before its clock is asked: the 16-bit setting, at a rate not known");

    check(!uac_rates_from_range(ranges, sizeof ranges - 1, &d.alts[0])
          && d.alts[0].nrates == 0, "a range a byte short is refused");

    for (i = 0; i < 3; i++) {
        check(uac_rates_from_range(ranges, sizeof ranges, &d.alts[i]),
              "its range is read");
    }

    check(d.alts[0].nrates == 6 && d.alts[0].rates[5] == 192000,
          "six rates, 44.1 to 192 kHz");

    i = uac_pick(&d, false, 44100, &rate);
    check(i == 0 && rate == 44100, "then a 44.1 kHz mix plays at 44.1 kHz");

    i = uac_pick_for(&d, false, 96000, 24, &rate);
    check(i == 1 && rate == 96000, "a 96/24 stream plays on 24 bits at 96 kHz");
    i = uac_pick_for(&d, false, 192000, 32, &rate);
    check(i == 2 && rate == 192000, "a 192/32 stream on 32 bits at 192 kHz");
    i = uac_pick_for(&d, false, 88200, 16, &rate);
    check(i == 0 && rate == 88200, "an 88.2/16 stream on 16 bits at 88.2 kHz");

    check(uac_rates_from_range(wide, sizeof wide, &d.alts[0])
          && d.alts[0].nrates == 10 && uac_takes(&d.alts[0], 44100)
          && uac_takes(&d.alts[0], 96000) && !uac_takes(&d.alts[0], 192000),
          "a range of every hertz offers the common rates inside it");
}

static void test_headset(void)
{
    struct uac_device d;
    const struct uac_alt *a;
    uint32_t rate = 0;
    int i;

    total(HEADSET, sizeof HEADSET);
    /* The control: the feedback endpoint made interrupt, so it is none. */
    corrupt(HEADSET, sizeof HEADSET, nth(HEADSET, sizeof HEADSET, 0x05, 0, 2) + 3, 0x03);

    uac_decode_config(HEADSET, sizeof HEADSET, &d);

    check(d.ok && !d.malformed, "the headset is audio, well formed");
    check(d.plays && d.records, "it plays and records");
    check(strcmp(uac_kind(&d), "headset") == 0, "it is a headset");
    check(d.output_type == 0x0402 && d.input_type == 0x0201,
          "a headset's earpiece and a microphone, the USB ends passed over");
    check(d.nalts == 2 && d.skipped == 1,
          "two settings carry PCM; the AC-3 one is skipped");

    a = &d.alts[0];
    check(!a->in && a->sync == UAC_SYNC_ASYNC, "the speaker is asynchronous");
    check(a->feedback == 2 && a->feedback_packet == 3,
          "its feedback is endpoint 2 IN, by bSynchAddress, 3 bytes");
    check(a->nrates == 2 && uac_takes(a, 44100) && uac_takes(a, 48000),
          "it takes 44.1 and 48 kHz");

    a = &d.alts[1];
    check(a->in && a->interface == 2 && a->endpoint == 3 && a->channels == 1,
          "the microphone: IN on endpoint 3, mono, on interface 2");
    check(a->continuous && uac_takes(a, 8000) && uac_takes(a, 44100)
          && !uac_takes(a, 96000), "8 to 48 kHz as a range");

    i = uac_pick(&d, false, 44100, &rate);
    check(i == 0 && rate == 44100, "plays at 44.1 kHz, as the mix is");
    i = uac_pick(&d, true, 44100, &rate);
    check(i == 1 && rate == 44100, "records at 44.1 kHz, inside its range");
}

static void test_m60(void)
{
    struct uac_device d;
    const struct uac_alt *a;
    uint32_t rate = 0;
    int i;

    check(sizeof M60 == 187, "the M60 is the 187 bytes it says");
    /* The control: its first setting's endpoint made bulk. */
    corrupt(M60, sizeof M60, nth(M60, sizeof M60, 0x05, 0, 1) + 3, 0x02);

    uac_decode_config(M60, sizeof M60, &d);

    check(d.ok && !d.malformed, "the M60 is audio, well formed");
    check(d.configuration == 1 && d.control == 0 && d.version == 1,
          "configuration 1, control on interface 0, UAC1");
    check(d.feature == 5 && d.output_type == 0x0301,
          "feature unit 5, playing into a speaker");
    check(d.plays && !d.records && strcmp(uac_kind(&d), "speaker") == 0,
          "a speaker that does not record");
    check(d.nalts == 2 && d.skipped == 0,
          "two settings carry sound; the HID interface is not one");

    a = &d.alts[0];
    check(a->interface == 1 && a->alternate == 1 && !a->in && a->endpoint == 3,
          "setting 1.1 plays on endpoint 3");
    check(a->sync == UAC_SYNC_SYNC && a->feedback == 0,
          "synchronous, so no feedback endpoint to follow");
    check(a->packet == 384 && a->interval == 1,
          "384 bytes a frame: 96 kHz of 16-bit stereo");
    check(a->channels == 2 && a->subframe == 2 && a->bits == 16
          && a->terminal == 4, "16-bit stereo, streaming to terminal 4");
    check(a->nrates == 2 && uac_takes(a, 48000) && uac_takes(a, 96000)
          && !uac_takes(a, 44100), "48 and 96 kHz, and not 44.1");
    check(a->sets_rate, "its rate chosen by SET_CUR to endpoint 3");

    a = &d.alts[1];
    check(a->alternate == 2 && a->subframe == 3 && a->bits == 24
          && a->packet == 576, "setting 1.2: 24 bits in 3 bytes, 576 a frame");

    i = uac_pick(&d, false, 44100, &rate);
    check(i == 0 && rate == 48000,
          "a 44.1 kHz mix plays on 1.1, 16 bits, at 48 kHz");

    /* U10d: the setting follows the best stream playing. */
    i = uac_pick_for(&d, false, 96000, 24, &rate);
    check(i == 1 && rate == 96000, "a 96/24 stream moves it to 1.2 at 96 kHz");
    i = uac_pick_for(&d, false, 48000, 16, &rate);
    check(i == 0 && rate == 48000, "a 48/16 stream keeps 1.1 at 48 kHz");
    i = uac_pick_for(&d, false, 96000, 16, &rate);
    check(i == 0 && rate == 96000, "a 96/16 stream: 1.1, at 96 kHz");
    i = uac_pick_for(&d, false, 44100, 24, &rate);
    check(i == 1 && rate == 48000,
          "a 44.1/24 stream: 1.2, at 48 kHz, the nearest above");
    i = uac_pick_for(&d, false, 192000, 32, &rate);
    check(i == 1 && rate == 96000,
          "a 192/32 stream: the most it has, 24 bits at 96 kHz");
}

/*
 * **The stream's arithmetic** (`uac_stream.c`, U2): a second of every rate
 * at full and high speed is the rate to the frame, no interval more than
 * one off another; and the tone is a sine at its pitch, laid out as the
 * setting lays samples out. The control asks for 1% less of each.
 */
static void test_stream(void)
{
    static const uint32_t RATES[] = { 8000, 22050, 32000, 44100, 48000,
                                      88200, 96000, 192000 };
    static const uint32_t UFRAMES[] = { 8, 1, 4 };
    struct uac_clock k;
    struct uac_tone t;
    uint8_t buffer[48 * 2 * 4];
    unsigned r, u, i, crossings = 0;
    int32_t most = 0, least = 0, last = 0, v;
    uint32_t cut = broken ? 100 : 0;

    for (r = 0; r < sizeof RATES / sizeof RATES[0]; r++) {
        for (u = 0; u < sizeof UFRAMES / sizeof UFRAMES[0]; u++) {
            uint32_t rate = RATES[r], uframes = UFRAMES[u];
            uint32_t intervals = 8000u / uframes, total = 0, f;
            uint32_t low = 0xFFFFFFFFu, high = 0;
            uint32_t top = uac_clock_most(rate, uframes);

            uac_clock_init(&k, rate - rate / 100 * (cut != 0), uframes);

            for (i = 0; i < intervals; i++) {
                f = uac_clock_next(&k);
                total += f;
                low = f < low ? f : low;
                high = f > high ? f : high;
            }

            check(total == rate && high - low <= 1 && high <= top,
                  "a second of a rate is the rate, to the frame");
        }
    }

    uac_clock_init(&k, 44100, 8);
    for (i = 0; i < 9; i++) {
        (void)uac_clock_next(&k);
    }
    check(uac_clock_next(&k) == 45 && uac_clock_most(44100, 8) == 45,
          "44.1 kHz at full speed: 44 nine times, 45 the tenth");

    uac_tone_init(&t, broken ? 436 : 440, 48000, 8192);
    /* 48001 samples: the 440th turn ends on sample 48000. */
    for (i = 0; i <= 48000; i++) {
        v = uac_tone_next(&t);
        most = v > most ? v : most;
        least = v < least ? v : least;
        crossings += (last < 0 && v >= 0) ? 1u : 0u;
        last = v;
    }
    check(most >= 8170 && most <= 8192 && least <= -8170 && least >= -8192,
          "the tone peaks at its level, within Bhaskara's 0.2%");
    check(crossings == 440, "440 Hz is 440 rising crossings in a second");

    uac_tone_init(&t, 1000, 48000, 16384);
    check(uac_tone_fill(&t, buffer, 48, 2, 2) == 192,
          "a millisecond of 16-bit stereo is 192 bytes");
    check(buffer[0] == 0 && buffer[1] == 0 && buffer[4] == buffer[6]
          && buffer[5] == buffer[7] && buffer[5] != 0,
          "starting at nought, both channels the same, little-endian");
    check(buffer[12 * 4 + 1] == 0x40 || buffer[12 * 4 + 1] == 0x3F,
          "a quarter turn in, its peak: 16384 is 0x4000");

    uac_tone_init(&t, 1000, 48000, 16384);
    check(uac_tone_fill(&t, buffer, 48, 2, 3) == 288,
          "a millisecond of 24-bit stereo is 288 bytes");
    check(buffer[12 * 6 + 0] == 0 && (buffer[12 * 6 + 2] == 0x40
                                      || buffer[12 * 6 + 2] == 0x3F),
          "24 bits: the 16-bit sample in the high two bytes");
}

static void test_not_audio(void)
{
    struct uac_device d;
    uint8_t zero[11] = { 9, 2, 11, 0, 1, 1, 0, 0x80, 50, 0, 4 };

    total(MIDI, sizeof MIDI);
    uac_decode_config(MIDI, sizeof MIDI, &d);
    check(!d.ok && d.nalts == 0 && !d.malformed,
          "a MIDI keyboard has AudioControl and no sound: not audio");
    check(strcmp(uac_kind(&d), "audio") == 0, "and is only \"audio\"");

    uac_decode_config(zero, sizeof zero, &d);
    check(d.malformed && !d.ok, "a descriptor of length zero stops it");
}

/*
 * ---- **The speaker's own volume** (U10g) ----
 */

/* A setup packet is what it should be, byte for byte. */
static int setup_is(const struct uac_setup *u, uint8_t type, uint8_t request,
                    uint16_t value, uint16_t index, uint16_t length)
{
    return u->request_type == type && u->request == request
        && u->value == value && u->index == index && u->length == length;
}

/* Every value `uac_volume_value` gives over a range, for every level, is
 * on the control's steps, inside it, at or below the top, and as near the
 * curve as a step allows: within half a step of it, or within one where it
 * had to come down a step to stay under the top. And louder never quieter
 * as the level rises. */
static void volume_ok_everywhere(const struct uac_volume_range *r, int16_t top,
                                 const char *what)
{
    int32_t ceiling = top < r->max ? top : r->max, want, v, last = -40000;
    unsigned level;
    int ok = 1;
    char line[128];

    ceiling = ceiling < r->min ? r->min : ceiling;

    for (level = 1; level <= 256; level++) {
        v = uac_volume_value(r, top, level);
        want = (int32_t)lround(top + 5120.0 * log10(level / 256.0));
        want = want < r->min ? r->min : want > ceiling ? ceiling : want;

        if (v < r->min || v > ceiling || v < last
            || (level < 256 && (v - r->min) % r->res != 0)
            || (level < 256 && labs((long)(v - want)) > r->res)
            || (level == 256 && v != ceiling)) {
            ok = 0;
            printf("  level %u: %d, the curve %d\n", level, v, want);
        }

        last = v;
    }

    snprintf(line, sizeof line, "%s: every level on a step, under the top, "
             "near the curve, rising", what);
    check(ok, line);
}

static void test_volume_curve(void)
{
    struct uac_volume_range fine;
    unsigned level, ok = 1, back = 1;

    /* A control of every 1/256 dB from -127.99 to +127.99: the curve
     * itself, each of its 256 numbers held to the logarithm. */
    check(uac_volume_range_v1(UAC_VOLUME_LEAST, 32767, 1, &fine),
          "a range of every 1/256 dB");

    for (level = 1; level <= 256; level++) {
        long want = lround(5120.0 * log10(level / 256.0));

        if (uac_volume_value(&fine, 0, level) != want) {
            ok = 0;
            printf("  level %u: %d, 20 log10 says %ld\n", level,
                   uac_volume_value(&fine, 0, level), want);
        }

        if (uac_volume_level(0, uac_volume_value(&fine, 0, level)) != level) {
            back = 0;
        }
    }

    check(ok, "the curve is 20 log10(level / 256) dB at every level");
    check(back, "and read back, every level is itself");
    check(uac_volume_value(&fine, 0, 128) == -1541
          && uac_volume_value(&fine, 0, 16) == -6165
          && uac_volume_value(&fine, 0, 1) == -12330,
          "-6.02 dB at 128, -24.08 at 16, -48.16 at 1");
    check(uac_volume_level(0, UAC_VOLUME_SILENCE) == 0
          && uac_volume_level(0, -20000) == 1
          && uac_volume_level(0, 0) == 256 && uac_volume_level(0, 600) == 256,
          "silence reads 0, quieter than 1 reads 1, the top and above 256");
    check(uac_volume_level(-1536, -1536) == 256
          && uac_volume_level(256, 256 - 1541) == 128,
          "and read against its top, wherever the top is");
    volume_ok_everywhere(&fine, 0, "every 1/256 dB, top at 0 dB");
}

/*
 * QEMU's speaker taking a SET_CUR of its volume, as `hw/usb/dev-audio.c`
 * does: the value less 0x8000, in sixteen bits, onto 0..255 by
 * (v x 255 + 0x4400) / 0x8800, and no more than 255. Its mixing engine
 * then scales each sample by that over 255. What `run_x86.py`'s
 * `usb_audio_exact_bits` expects to hear at the master's 256 and 128 on
 * the top `xhci` gives QEMU's speaker, its MAX (U10g).
 */
static unsigned qemu_takes(int16_t value)
{
    uint16_t v = (uint16_t)((uint16_t)value - 0x8000u);
    unsigned vol = ((unsigned)v * 255u + 0x4400u) / 0x8800u;

    return vol > 255u ? 255u : vol;
}

static void test_volume_qemu_heard(void)
{
    struct uac_volume_range r;
    int16_t top;

    check(uac_volume_range_v1((int16_t)0x8001, 0x0800, 0x0088, &r), "QEMU's range again");
    top = r.max;

    check(qemu_takes(0) == 240, "QEMU's 0 dB is 240 of 255 (testing.md 18.1014)");
    check(qemu_takes(uac_volume_value(&r, top, 256)) == 255,
          "the master's 256 on QEMU's top is its 255, unity: the noise bit for bit");
    check(uac_volume_value(&r, top, 128) == 553 && qemu_takes(553) == 244,
          "the master's 128 is 553, which QEMU takes as 244: 0.957 of the sample, "
          "inside run_x86.py's 0.93 to 0.98");
    check(qemu_takes(uac_volume_value(&r, top, 128)) * 128u / 256u < 0.93 * 255,
          "and the master left in the mix as well would be 0.48, outside it: "
          "the part's control");
}

static void test_volume_qemu(void)
{
    struct uac_device d;
    struct uac_setup u;
    struct uac_volume_range r;
    struct uac_volume_set set;
    uint8_t ch[4], two[2];
    int16_t unity;

    uac_decode_config(QEMU, sizeof QEMU, &d);

    /* Feature unit 2: bmaControls(0) 0x0001, a mute on the master;
     * (1) and (2) 0x0002, a volume on each channel (dev-audio.c). */
    check(d.volume.unit == 2 && d.volume.source == 1 && d.volume.channels == 2,
          "QEMU's speaker: unit 2, from terminal 1, two channels");
    check(d.volume.mute == UAC_CONTROL_MASTER
          && d.volume.volume == UAC_CONTROL_CHANNELS
          && d.volume.volume_channels == 0x3 && !d.volume.read_only,
          "a mute on the master, a volume on each channel and none on it");
    check(uac_fu_channels(&d.volume, UAC_FU_VOLUME, ch, 4) == 2
          && ch[0] == 1 && ch[1] == 2, "so the volume is set on 1 and 2");
    check(uac_fu_channels(&d.volume, UAC_FU_MUTE, ch, 4) == 1 && ch[0] == 0,
          "and the mute on the master");
    check(uac_fu_channels(&d.volume, UAC_FU_VOLUME, ch, 1) == 1 && ch[0] == 1,
          "as many as there is room for");

    /* Its requests: wIndex 0x0200, unit 2 on interface 0. */
    check(uac_fu_request(&d, UAC_FU_VOLUME, UAC_ATTR_MIN, false, 1, &u)
          && setup_is(&u, 0xA1, 0x82, 0x0201, 0x0200, 2), "GET_MIN, channel 1");
    check(uac_fu_request(&d, UAC_FU_VOLUME, UAC_ATTR_MAX, false, 2, &u)
          && setup_is(&u, 0xA1, 0x83, 0x0202, 0x0200, 2), "GET_MAX, channel 2");
    check(uac_fu_request(&d, UAC_FU_VOLUME, UAC_ATTR_RES, false, 1, &u)
          && setup_is(&u, 0xA1, 0x84, 0x0201, 0x0200, 2), "GET_RES");
    check(uac_fu_request(&d, UAC_FU_VOLUME, UAC_ATTR_CUR, false, 1, &u)
          && setup_is(&u, 0xA1, 0x81, 0x0201, 0x0200, 2), "GET_CUR");
    check(uac_fu_request(&d, UAC_FU_VOLUME, UAC_ATTR_CUR, true, 2, &u)
          && setup_is(&u, 0x21, 0x01, 0x0202, 0x0200, 2), "SET_CUR of a volume");
    check(uac_fu_request(&d, UAC_FU_MUTE, UAC_ATTR_CUR, true, 0, &u)
          && setup_is(&u, 0x21, 0x01, 0x0100, 0x0200, 1), "SET_CUR of the mute");
    check(uac_fu_request(&d, UAC_FU_MUTE, UAC_ATTR_CUR, false, 0, &u)
          && setup_is(&u, 0xA1, 0x81, 0x0100, 0x0200, 1), "GET_CUR of the mute");
    u.request = 0x5A;
    check(!uac_fu_request(&d, UAC_FU_VOLUME, UAC_ATTR_RANGE, false, 1, &u)
          && !uac_fu_request(&d, UAC_FU_MUTE, UAC_ATTR_MAX, false, 0, &u)
          && !uac_fu_request(&d, UAC_FU_VOLUME, UAC_ATTR_MAX, true, 1, &u)
          && !uac_fu_request(&d, UAC_FU_VOLUME, UAC_ATTR_CUR, false, 3, &u)
          && !uac_fu_request(&d, 0x03, UAC_ATTR_CUR, false, 0, &u)
          && u.request == 0x5A,
          "no RANGE on UAC1, no MAX of a mute, no SET_MAX, no channel 3, "
          "no bass - and nothing written for them");

    uac_volume_put(-12367, two);
    check(two[0] == 0xB1 && two[1] == 0xCF && uac_volume_get(two) == -12367,
          "a volume's two bytes, signed, little-endian");
    two[0] = 0x00; two[1] = 0x80;
    check(uac_volume_get(two) == UAC_VOLUME_SILENCE, "0x8000 is silence");

    /* Its range, as GET_MIN, GET_MAX and GET_RES answer: 0x8001, 0x0800
     * and 0x0088 - -127.996 dB to +8 dB in 0.53 dB steps. */
    check(uac_volume_range_v1((int16_t)0x8001, 0x0800, 0x0088, &r)
          && r.min == -32767 && r.max == 2048 && r.res == 136,
          "QEMU's range: -128 to +8 dB, in steps of 136/256 dB");
    unity = uac_volume_unity(&r);
    check(unity == 0, "its unity is 0 dB, inside it");

    /* On the steps from MIN: -32767 + 136k. 0 dB is between two. */
    check(uac_volume_value(&r, unity, 256) == 0,
          "level 256 is 0 dB exactly, though no step lands there");
    check(uac_volume_value(&r, unity, 128) == -1487,
          "level 128, -6.02 dB, is the step at -5.81");
    check(uac_volume_value(&r, unity, 100) == -2031,
          "level 100, -8.16 dB, the step at -7.93");
    check(uac_volume_value(&r, unity, 50) == -3663,
          "level 50, -14.18 dB, the step at -14.31");
    check(uac_volume_value(&r, unity, 1) == -12367,
          "level 1, -48.16 dB, the step at -48.31");
    check(uac_volume_value(&r, unity, 0) == -32767, "level 0 is MIN");
    volume_ok_everywhere(&r, unity, "QEMU's, top at 0 dB");

    /* With its top as the master's top - QEMU's 255 of 255, which passes
     * samples untouched (U10f). */
    check(uac_volume_value(&r, r.max, 256) == 2048
          && uac_volume_value(&r, r.max, 128) == 553,
          "top at +8 dB: 256 is +8, 128 is +2.16, the step under +1.98");
    volume_ok_everywhere(&r, r.max, "QEMU's, top at its MAX");
    check(uac_volume_level(unity, -1487) == 131 && uac_volume_level(unity, 0) == 256
          && uac_volume_level(unity, -32767) == 1,
          "read back: -5.81 dB is 131, 0 dB 256, MIN 1");

    uac_volume_for(&d, &r, unity, 128, false, &set);
    check(!set.mute && set.value == -1487 && !set.silent, "128: its value, heard");
    uac_volume_for(&d, &r, unity, 0, false, &set);
    check(set.mute && set.value == -32767 && set.silent,
          "0: the mute, which silences it");
    uac_volume_for(&d, &r, unity, 128, true, &set);
    check(set.mute && set.value == -1487 && set.silent,
          "muted at 128: the mute, and 128's value kept for unmuting");
}

static void test_volume_master(void)
{
    struct uac_device d;
    struct uac_setup u;
    uint8_t ch[4];

    /* The M60's unit 5: bmaControls(0) 0x0003, a mute and a volume on
     * the master; its channels none. */
    uac_decode_config(M60, sizeof M60, &d);
    check(d.volume.unit == 5 && d.volume.channels == 2
          && d.volume.volume == UAC_CONTROL_MASTER
          && d.volume.mute == UAC_CONTROL_MASTER,
          "the M60: unit 5, a volume and a mute on its master");
    check(uac_fu_channels(&d.volume, UAC_FU_VOLUME, ch, 4) == 1 && ch[0] == 0,
          "set on channel 0 alone");
    check(uac_fu_request(&d, UAC_FU_VOLUME, UAC_ATTR_CUR, true, 0, &u)
          && setup_is(&u, 0x21, 0x01, 0x0200, 0x0500, 2),
          "SET_CUR of the M60's volume: 21 01 0200 0500 0002");

    /* The DAC's unit 0x0A: bmaControls(0) 0xF - mute and volume, each
     * 0b11, programmable; its channels 0xC, a volume each. */
    uac_decode_config(DAC, sizeof DAC, &d);
    check(d.volume.unit == 0x0A && d.volume.source == 0x02
          && d.volume.channels == 2 && d.volume.volume == UAC_CONTROL_MASTER
          && d.volume.mute == UAC_CONTROL_MASTER && !d.volume.read_only,
          "the DAC: unit 0x0A, a volume and a mute on the master, UAC2");
    check(uac_fu_request(&d, UAC_FU_VOLUME, UAC_ATTR_RANGE, false, 0, &u)
          && setup_is(&u, 0xA1, 0x02, 0x0200, 0x0A00, 2 + 6 * UAC_VOLUME_SUBRANGES_MAX),
          "UAC2 GET RANGE: A1 02 0200 0A00, room for eight subranges");
    check(uac_fu_request(&d, UAC_FU_VOLUME, UAC_ATTR_CUR, false, 0, &u)
          && setup_is(&u, 0xA1, 0x01, 0x0200, 0x0A00, 2), "UAC2 GET CUR");
    check(uac_fu_request(&d, UAC_FU_VOLUME, UAC_ATTR_CUR, true, 0, &u)
          && setup_is(&u, 0x21, 0x01, 0x0200, 0x0A00, 2), "UAC2 SET CUR");
    check(uac_fu_request(&d, UAC_FU_MUTE, UAC_ATTR_CUR, true, 0, &u)
          && setup_is(&u, 0x21, 0x01, 0x0100, 0x0A00, 1), "UAC2 SET CUR of the mute");
    check(!uac_fu_request(&d, UAC_FU_VOLUME, UAC_ATTR_MIN, false, 0, &u)
          && !uac_fu_request(&d, UAC_FU_VOLUME, UAC_ATTR_RES, false, 0, &u)
          && !uac_fu_request(&d, UAC_FU_MUTE, UAC_ATTR_RANGE, false, 0, &u),
          "no MIN or RES on UAC2, and no RANGE of a mute");
}

static void test_volume_range_v2(void)
{
    struct uac_device d;
    struct uac_volume_range r;
    struct uac_volume_set set;
    /* -50 to -30 dB in 2 dB steps, then -29 to +6 dB in half decibels:
     * the second covers most. */
    static const uint8_t two[] = {
        U16(2), U16(-12800), U16(-7680), U16(512),
                U16(-7424), U16(1536), U16(128),
    };
    /* A MIN of 0x8000, which 2.0 5.2.5.7.2 forbids, and one subrange the
     * wrong way round before it. */
    static const uint8_t odd[] = {
        U16(2), U16(0), U16(-256), U16(1),
                U16(0x8000), U16(0), U16(0),
    };
    uint8_t broken_two[sizeof two];

    memcpy(broken_two, two, sizeof two);
    /* The control: the first subrange made the widest. */
    if (broken) {
        broken_two[2] = 0x00; broken_two[3] = 0x80;
    }

    check(uac_volume_range_v2(broken_two, sizeof two, &r)
          && r.min == -7424 && r.max == 1536 && r.res == 128,
          "two subranges: the one that covers most, -29 to +6 dB");
    check(uac_volume_range_v2(two, 8, &r) && r.min == -12800 && r.max == -7680
          && r.res == 512, "cut to wLength: the subrange whole in it");
    check(!uac_volume_range_v2(two, 7, &r) && !uac_volume_range_v2(two, 1, &r),
          "no subrange whole: refused");
    check(uac_volume_range_v2(odd, sizeof odd, &r) && r.min == -32767
          && r.max == 0 && r.res == 1,
          "MIN above MAX passed over; MIN 0x8000 taken as 0x8001, RES 0 as 1");

    check(uac_volume_range_v2(two, sizeof two, &r), "the range again");
    check(uac_volume_unity(&r) == 0, "unity 0 dB, inside it");
    check(uac_volume_value(&r, 0, 256) == 0 && uac_volume_value(&r, 0, 128) == -1536
          && uac_volume_value(&r, 0, 50) == -3584
          && uac_volume_value(&r, 0, 100) == -2048
          && uac_volume_value(&r, 0, 1) == -7424,
          "256 0 dB, 128 -6, 100 -8, 50 -14 and 1 its MIN, -29");
    volume_ok_everywhere(&r, 0, "the DAC's, top at 0 dB");
    check(uac_volume_level(0, -7424) == 9,
          "its MIN reads 9 - where -29 dB is on the curve - not 1");

    uac_decode_config(DAC, sizeof DAC, &d);
    uac_volume_for(&d, &r, 0, 0, false, &set);
    check(set.mute && set.silent, "0 on the DAC: its mute");

    /* A range all below 0 dB: unity is its MAX; all above, its MIN. */
    check(uac_volume_range_v1(-6000, -1000, 100, &r) && uac_volume_unity(&r) == -1000
          && uac_volume_range_v1(100, 900, 1, &r) && uac_volume_unity(&r) == 100,
          "unity clamped into a range that does not hold 0 dB");
    check(!uac_volume_range_v1(10, -10, 1, &r), "MIN above MAX refused");
}

static void test_volume_channels(void)
{
    struct uac_device d;
    struct uac_volume_range r;
    struct uac_volume_set set;
    struct uac_setup u;
    uint8_t ch[4];

    total(PERCH, sizeof PERCH);
    /* The control: channel 2's volume bit cleared, so the volume is on
     * one channel of two, which is not used. */
    corrupt(PERCH, sizeof PERCH, nth(PERCH, sizeof PERCH, 0x24, 0x06, 2) + 8, 0x00);

    uac_decode_config(PERCH, sizeof PERCH, &d);
    check(d.ok && d.plays && d.output_type == 0x0301,
          "the speaker with its volume on its channels: a speaker");
    check(d.feature == 5 && d.volume.unit == 7,
          "the first unit is the microphone's; the speaker's volume is unit 7, "
          "behind unit 8's mute");
    check(d.volume.volume == UAC_CONTROL_CHANNELS && d.volume.volume_channels == 0x3
          && d.volume.mute == UAC_CONTROL_NONE && d.volume.channels == 2,
          "a volume on channels 1 and 2 and none on the master; no mute in it");
    check(uac_fu_channels(&d.volume, UAC_FU_VOLUME, ch, 4) == 2 && ch[0] == 1
          && ch[1] == 2 && uac_fu_channels(&d.volume, UAC_FU_MUTE, ch, 4) == 0,
          "set on both channels the same; no mute to set");
    check(uac_fu_request(&d, UAC_FU_VOLUME, UAC_ATTR_CUR, true, 2, &u)
          && setup_is(&u, 0x21, 0x01, 0x0202, 0x0700, 2), "SET_CUR on unit 7, channel 2");

    /* -60 to 0 dB in whole decibels. */
    check(uac_volume_range_v1(-15360, 0, 256, &r), "-60 to 0 dB by 1 dB");
    check(uac_volume_value(&r, 0, 256) == 0 && uac_volume_value(&r, 0, 128) == -1536
          && uac_volume_value(&r, 0, 100) == -2048 && uac_volume_value(&r, 0, 50) == -3584
          && uac_volume_value(&r, 0, 1) == -12288,
          "256 0 dB, 128 -6, 100 -8, 50 -14, 1 -48");
    volume_ok_everywhere(&r, 0, "-60 to 0 by 1 dB");

    uac_volume_for(&d, &r, 0, 0, false, &set);
    check(!set.mute && set.value == UAC_VOLUME_SILENCE && !set.silent,
          "0 with no mute on UAC1: 0x8000, and the server silent as well");
    uac_volume_for(&d, &r, 0, 200, true, &set);
    check(!set.mute && set.value == UAC_VOLUME_SILENCE && !set.silent,
          "muted, the same");
}

static void test_volume_none(void)
{
    struct uac_device d;
    struct uac_setup u;
    uint8_t ch[4];
    uint8_t ro[sizeof DAC];
    unsigned at;

    /* The headset: its earpiece's terminal 3 takes its sound from
     * terminal 1, no unit between. */
    uac_decode_config(HEADSET, sizeof HEADSET, &d);
    check(d.volume.unit == 0 && d.feature == 0 && !d.volume.read_only,
          "the headset has no volume of its own: the master stays in the mix");
    check(!uac_fu_request(&d, UAC_FU_VOLUME, UAC_ATTR_CUR, true, 0, &u)
          && uac_fu_channels(&d.volume, UAC_FU_VOLUME, ch, 4) == 0,
          "and nothing to ask it");

    /* The DAC with its unit's controls read-only: 0b01 each. */
    memcpy(ro, DAC, sizeof DAC);
    at = nth(ro, sizeof ro, 0x24, 0x06, 1);
    ro[at + 5] = 0x05;
    ro[at + 9] = 0x04;
    ro[at + 13] = 0x04;
    uac_decode_config(ro, sizeof ro, &d);
    check(d.feature == 0x0A && d.volume.unit == 0 && d.volume.read_only,
          "a UAC2 unit whose volume and mute are read-only carries no master, "
          "and says why");

    /* One channel of two, programmable, the other read-only: neither. */
    ro[at + 5] = 0x00;
    ro[at + 9] = 0x0C;
    uac_decode_config(ro, sizeof ro, &d);
    check(d.volume.unit == 0, "a volume on one channel of two is not used");
}

/* ---- Every length, at the edge of an unmapped page. */
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

static void cut_short(const char *name, const uint8_t *bytes, unsigned size)
{
    unsigned n, boundaries = 0;
    struct uac_device d;
    char what[96];

    for (n = 0; n <= size; n++) {
        unsigned at = 0;
        int boundary;

        while (at < n) {
            at += bytes[at];
        }

        boundary = (at == n);
        boundaries += boundary;
        uac_decode_config(on_the_edge(bytes, n), n, &d);

        if (boundary == d.malformed) {
            snprintf(what, sizeof what, "%s cut at %u: malformed is %d",
                     name, n, d.malformed);
            check(0, what);
        }
    }

    snprintf(what, sizeof what, "every length of %s, read in bounds", name);
    check(boundaries > 5, what);
}

int main(void)
{
    broken = getenv("UACTEST_BROKEN") != NULL;

    test_qemu();
    test_qemu_multi();
    test_dac();
    test_headset();
    test_m60();
    test_stream();
    test_not_audio();
    test_volume_curve();
    test_volume_qemu();
    test_volume_qemu_heard();
    test_volume_master();
    test_volume_range_v2();
    test_volume_channels();
    test_volume_none();

    if (!broken) {
        cut_short("QEMU's speaker", QEMU, sizeof QEMU);
        cut_short("QEMU's multi-channel speaker", QEMU_MULTI, sizeof QEMU_MULTI);
        cut_short("the DAC", DAC, sizeof DAC);
        cut_short("the headset", HEADSET, sizeof HEADSET);
        cut_short("the M60", M60, sizeof M60);
        cut_short("the MIDI keyboard", MIDI, sizeof MIDI);
        cut_short("the speaker with a volume on each channel", PERCH, sizeof PERCH);
    }

    if (fails) {
        printf("FAIL: %d of %d checks on USB Audio Class decoding%s\n",
               fails, checks + fails, broken ? " (the control, as it should)" : "");
        return 1;
    }

    printf("PASS: %d checks on USB Audio Class decoding (QEMU's speaker byte "
           "for byte and its 5.1 and 7.1, a UAC2 DAC and its clock's rates, "
           "a UAC1 headset with feedback, Diego's EDIFIER M60, a second of every rate and the tone, a MIDI keyboard, "
           "each one's own volume and its requests and the master's curve onto it, every length cut "
           "short)\n", checks);
    return 0;
}
