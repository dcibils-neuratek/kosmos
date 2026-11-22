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
 *   - every fixture cut short at every length, each copy ending on the last
 *     byte before a page that is not mapped, so a read past the end is a
 *     crash rather than a pass.
 *
 * Its control: `UACTEST_BROKEN=1` corrupts one field in each fixture before
 * decoding, and the run must fail.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../user/drivers/usb/uac_decode.h"

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
    test_not_audio();

    if (!broken) {
        cut_short("QEMU's speaker", QEMU, sizeof QEMU);
        cut_short("QEMU's multi-channel speaker", QEMU_MULTI, sizeof QEMU_MULTI);
        cut_short("the DAC", DAC, sizeof DAC);
        cut_short("the headset", HEADSET, sizeof HEADSET);
        cut_short("the MIDI keyboard", MIDI, sizeof MIDI);
    }

    if (fails) {
        printf("FAIL: %d of %d checks on USB Audio Class decoding%s\n",
               fails, checks + fails, broken ? " (the control, as it should)" : "");
        return 1;
    }

    printf("PASS: %d checks on USB Audio Class decoding (QEMU's speaker byte "
           "for byte and its 5.1 and 7.1, a UAC2 DAC and its clock's rates, "
           "a UAC1 headset with feedback, a MIDI keyboard, every length cut "
           "short)\n", checks);
    return 0;
}
