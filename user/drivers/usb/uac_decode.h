/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DRIVERS_USB_UAC_DECODE_H
#define KOSMOS_DRIVERS_USB_UAC_DECODE_H

/*
 * **A USB Audio Class device, as its bytes describe it** (`docs/usbaudio.md`,
 * U1): whether it plays, records or both, which alternate settings of its
 * streaming interfaces carry sound, in what sample size, channels and rates,
 * on which isochronous endpoint, kept in time how - and, for an
 * asynchronous one, where it says how fast it wants to be fed.
 *
 * Its own file with no hardware and no system calls in it, as
 * `uvc_decode.c` is: the bytes are the device's to choose, so
 * `tools/test_uacdecode.c` holds this to QEMU's own speaker's, to a UAC2
 * DAC's laid out as the specification lays one out, and to the broken ones
 * no real device sends.
 *
 * The specifications are USB Audio 1.0 (4.3 and 4.5 for the interfaces,
 * 4.6 for the endpoints; Frmts10 2.2.5 for Type I formats) and USB Audio
 * 2.0 (4.7 and 4.9, 4.10; FMT20 2.3.1.6). The two lay their descriptors out
 * differently under the same subtypes, and an interface's bInterfaceProtocol
 * - 0x00 or 0x20 - says which one it speaks.
 */

#include <stdbool.h>
#include <stdint.h>

/* Room for this many settings in one device, and rates in one setting. A
 * speaker has one or two settings; a DAC a dozen. More are counted, not kept. */
#define UAC_ALTS_MAX    24
#define UAC_RATES_MAX   16

/* How an isochronous endpoint keeps time (USB 2.0 9.6.6, bmAttributes 3:2). */
enum uac_sync {
    UAC_SYNC_NONE     = 0,
    UAC_SYNC_ASYNC    = 1,      /* the device's clock; it says the rate on a
                                   feedback endpoint, or implicitly */
    UAC_SYNC_ADAPTIVE = 2,      /* the device follows whatever the host sends */
    UAC_SYNC_SYNC     = 3,      /* locked to the bus's start of frame */
};

/* One alternate setting that carries PCM, and its endpoint. */
struct uac_alt {
    uint8_t  interface;         /* bInterfaceNumber, for SET_INTERFACE */
    uint8_t  alternate;         /* bAlternateSetting */
    bool     in;                /* true: it records; false: it plays */
    uint8_t  endpoint;          /* its number, 1 to 15 */
    uint8_t  sync;              /* enum uac_sync */
    uint16_t packet;            /* wMaxPacketSize 10:0 */
    uint8_t  extra;             /* wMaxPacketSize 12:11: more transactions */
    uint8_t  interval;          /* bInterval, as the device said it */
    bool     sets_rate;         /* UAC1: its rate is chosen by SET_CUR of
                                   the endpoint's sampling frequency (its
                                   class endpoint descriptor's bit 0, 4.6.1.2);
                                   asked of one that has not said so, it
                                   would stall */

    /* An asynchronous output's feedback endpoint, IN, or 0 for none. */
    uint8_t  feedback;
    uint16_t feedback_packet;
    uint8_t  feedback_interval;

    uint8_t  terminal;          /* bTerminalLink: the terminal it streams to */
    uint8_t  channels;          /* bNrChannels */
    uint8_t  subframe;          /* bytes a sample takes: 1 to 4 */
    uint8_t  bits;              /* how many of them are sound */

    /*
     * The rates it takes. UAC1 says them here: a list, or - `continuous` -
     * a range from rates[0] to rates[1]. UAC2 says them through its clock
     * source when asked (`uac_rates_from_range`), so `nrates` starts at 0.
     */
    bool     continuous;
    uint8_t  nrates;
    uint32_t rates[UAC_RATES_MAX];
};

/*
 * **The volume and mute a speaker has of its own** (`docs/usbaudio.md`
 * U10g): the feature unit on the path to the output terminal `output_type`
 * names, and which of its channels carry a volume control and a mute.
 *
 * A control is on the master channel 0, on each logical channel, or
 * absent. The master is preferred; where only the logical channels have
 * one, every one of them is set to the same value, so the speaker's
 * balance is not moved (QEMU's speaker is that: a master mute and a
 * volume on each of its two channels, `hw/usb/dev-audio.c`).
 *
 * UAC1 says a control is there or not (Audio 1.0, 4.3.2.5, Table 4-7:
 * bmaControls of bControlSize bytes, D0 mute, D1 volume; "audio class v1
 * controls are never read-only", Linux `sound/usb/mixer.c`). UAC2 says it
 * in pairs of bits (Audio 2.0, 4.7.2.8, Table 4-13: four bytes a channel,
 * D1..0 mute, D3..2 volume; 0b01 present and read-only, 0b11 host
 * programmable, 0b10 not allowed). Only a programmable one is used here: a
 * volume that can be read and not set cannot carry the master.
 */
enum uac_where {
    UAC_CONTROL_NONE     = 0,
    UAC_CONTROL_MASTER   = 1,   /* channel 0 */
    UAC_CONTROL_CHANNELS = 2,   /* each logical channel in `*_channels` */
};

#define UAC_FU_MUTE         0x01u   /* MUTE_CONTROL: Audio 1.0 A.10.2, 2.0 A.17.7 */
#define UAC_FU_VOLUME       0x02u   /* VOLUME_CONTROL */
#define UAC_FEATURES_MAX    8u      /* feature units remembered on the walk */
#define UAC_FU_CHANNELS_MAX 32u     /* logical channels a mask can name */

struct uac_feature {
    uint8_t  unit;              /* bUnitID, or 0: none on the path */
    uint8_t  source;            /* bSourceID */
    uint8_t  channels;          /* logical channels it has bitmaps for */
    uint8_t  volume;            /* enum uac_where */
    uint8_t  mute;              /* enum uac_where */
    bool     read_only;         /* UAC2: a volume or mute it will not take -
                                   said even where `unit` is 0, for a path
                                   whose only controls are read-only */
    uint32_t volume_channels;   /* bit n-1: logical channel n has a volume */
    uint32_t mute_channels;     /* and a mute */
};

struct uac_device {
    bool     ok;                /* an AudioControl interface, and a setting
                                   that carries PCM on an isochronous endpoint */
    bool     malformed;         /* a length that walks off the end, or zero */
    uint8_t  configuration;     /* bConfigurationValue */
    uint8_t  control;           /* the AudioControl interface */
    uint8_t  version;           /* 1 or 2: the AudioControl interface's */
    uint16_t adc;               /* bcdADC: 0x0100 or 0x0200 */
    uint8_t  clock;             /* UAC2: the first clock source's ID, or 0 */
    uint8_t  feature;           /* the first feature unit's ID, or 0 */

    /* The feature unit nearest the output terminal, upstream, that has a
     * volume (else a mute) it will take: what the master drives (U10g).
     * `volume.unit` is 0 where there is none, and the master stays in the
     * mix. Only feature units are walked through: a mixer, a selector or
     * a processing unit ends the walk. */
    struct uac_feature volume;

    /* The kind of what it plays into and records from: the first output
     * terminal and input terminal that are not the USB end (0x0101), as
     * wTerminalType says - 0x0301 a speaker, 0x0302 headphones, 0x0201 a
     * microphone, 0x0402 a headset. 0 where it has none. */
    uint16_t output_type;
    uint16_t input_type;

    bool     plays;             /* a setting that plays */
    bool     records;           /* a setting that records */

    unsigned nalts;
    struct uac_alt alts[UAC_ALTS_MAX];
    unsigned alts_dropped;      /* settings past UAC_ALTS_MAX */
    unsigned rates_dropped;     /* rates past UAC_RATES_MAX, in any setting */
    unsigned skipped;           /* settings that carry something not read
                                   here: not PCM, not Type I, no endpoint */
};

/*
 * `bytes` is what GET_DESCRIPTOR returned for the configuration and `length`
 * how many of them arrived. Nothing outside those is read, and a descriptor
 * whose length is zero or runs past the end stops the walk with `malformed`
 * set and what came before it kept.
 */
void uac_decode_config(const uint8_t *bytes, unsigned length,
                       struct uac_device *out);

/* What it is, as a word a person reads: "headset" when it plays and
 * records; "speaker" or "headphones" by its output terminal, "microphone"
 * by its input terminal; "audio output" or "audio input" when the terminal
 * is something else, a line out or an S/PDIF; "audio" when it is neither. */
const char *uac_kind(const struct uac_device *d);

/* Whether `a` takes `rate`. */
bool uac_takes(const struct uac_alt *a, uint32_t rate);

/*
 * The setting to play (`in` false) or record on by default, its index in
 * `d->alts`, and in `*rate` the rate to run it at - or -1. The best it
 * offers that a mix of 16-bit samples at `mix_rate` loses nothing to
 * (`usbaudio.md`): two channels before more and more before one; the
 * fewest bits of at least 16; `mix_rate` itself if it takes it, else the
 * nearest rate above it, else its highest. `*rate` is 0 for a setting that
 * has not said its rates yet - a UAC2 one, before its clock is asked.
 */
int uac_pick(const struct uac_device *d, bool in, uint32_t mix_rate,
             uint32_t *rate);

/*
 * The same for a stream of `want_bits` at `want_rate` (`usbaudio.md` U10d):
 * two channels first, as above; then a setting with no fewer bits than
 * the stream, since widening is exact and narrowing is not; then the
 * stream's own rate, else the nearest above, else the highest below; then
 * the fewest bits past the stream's. `uac_pick` is this for 16 bits.
 */
int uac_pick_for(const struct uac_device *d, bool in, uint32_t want_rate,
                 unsigned want_bits, uint32_t *rate);

/*
 * A UAC2 clock source's answer to GET RANGE for its sampling frequency
 * (UAC2 5.2.3.3): wNumSubRanges, then each its dMIN, dMAX and dRES. Into
 * `a->rates` the ones it takes: a subrange of one rate as itself, and a
 * wider one as the common rates inside it that its steps land on, as many
 * as UAC_RATES_MAX holds. False, and `a` untouched, for a block that is
 * short of what it says it holds.
 */
bool uac_rates_from_range(const uint8_t *bytes, unsigned length,
                          struct uac_alt *a);

/*
 * ---- **The speaker's own volume, asked and set** (U10g) ----
 *
 * A class request to a feature unit, as its eight setup bytes say it:
 * bmRequestType 0xA1 to read and 0x21 to write (class, interface; Audio
 * 1.0 5.2.2.4.1-2, Tables 5-14 and 5-15; Audio 2.0 5.2.2, Table 5-1);
 * wValue the control selector in the high byte and the channel in the low;
 * wIndex the unit in the high byte and the AudioControl interface in the
 * low (1.0 5.2.1.1; 2.0 5.2.2).
 */
struct uac_setup {
    uint8_t  request_type;
    uint8_t  request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
};

/* Which attribute. UAC1 has CUR, MIN, MAX and RES (A.9: 0x01 SET_CUR,
 * 0x81-0x84 GET_CUR..GET_RES); UAC2 has CUR and RANGE (A.14: 0x01 and
 * 0x02, the direction in bmRequestType), RANGE being MIN, MAX and RES as
 * triplets, never apart (5.2.1). */
enum uac_attribute {
    UAC_ATTR_CUR   = 0,
    UAC_ATTR_MIN   = 1,
    UAC_ATTR_MAX   = 2,
    UAC_ATTR_RES   = 3,
    UAC_ATTR_RANGE = 4,
};

/* Subranges a GET RANGE of the volume leaves room for: 2 + 6 x this
 * bytes. A device with more sends the first ones only (2.0 5.2.2). */
#define UAC_VOLUME_SUBRANGES_MAX 8u

/*
 * The request for `attribute` of `selector` (UAC_FU_MUTE or UAC_FU_VOLUME)
 * on channel `channel` of `d->volume`'s unit: a read, or with `set` a
 * SET_CUR. False, and `out` untouched, for one the device's version has
 * no such request for (RANGE on UAC1; MIN, MAX or RES on UAC2), anything
 * but CUR of a mute (1.0 5.2.2.4.3.1, 2.0 5.2.5.7.1: a mute has only
 * CUR), a set of anything but CUR, a channel past the unit's, or no unit.
 * wLength is the parameter block's: 1 for a mute, 2 for a volume, and
 * 2 + 6 x UAC_VOLUME_SUBRANGES_MAX for a UAC2 volume's RANGE.
 */
bool uac_fu_request(const struct uac_device *d, unsigned selector,
                    enum uac_attribute attribute, bool set, uint8_t channel,
                    struct uac_setup *out);

/*
 * The channels to address a control on: {0} where it is on the master,
 * else each logical channel that has one, in order, as many as `room`.
 * Returns how many; 0 for none.
 */
unsigned uac_fu_channels(const struct uac_feature *f, unsigned selector,
                         uint8_t *channels, unsigned room);

/* A volume's two bytes, signed and little-endian (1.0 Table 5-18, 2.0
 * Table 5-4), each way. */
void    uac_volume_put(int16_t value, uint8_t out[2]);
int16_t uac_volume_get(const uint8_t in[2]);

/*
 * **A volume control's range**, in its own units: a signed 1/256 dB, from
 * +127.9961 dB (0x7FFF) down to -127.9961 dB (0x8001); 0x8000 is silence,
 * -infinity, as a CUR only (1.0 5.2.2.4.3.2; 2.0 5.2.5.7.2, which says it
 * "must always be implemented" and is never reported as the MIN).
 */
#define UAC_VOLUME_SILENCE  ((int16_t)-32768)   /* 0x8000 */
#define UAC_VOLUME_LEAST    ((int16_t)-32767)   /* 0x8001 */

struct uac_volume_range {
    int16_t min, max, res;      /* res at least 1 */
};

/*
 * UAC1's three answers - GET_MIN, GET_MAX, GET_RES - as a range. A MIN of
 * 0x8000, which the specification does not allow, is taken as 0x8001; a
 * RES of nought or less, or a device that would not say, as 1 (Linux's
 * `get_ctl_range_v1` takes 1 when GET_RES fails). False for a MIN above
 * the MAX.
 */
bool uac_volume_range_v1(int16_t min, int16_t max, int16_t res,
                         struct uac_volume_range *out);

/*
 * UAC2's answer to GET RANGE of a volume (2.0 5.2.3.2, Table 5-5):
 * wNumSubRanges, then each its wMIN, wMAX and wRES. The subrange that
 * covers most is taken - a range with gaps in it is used for its widest
 * part. A block shorter than it says is read for the subranges whole in it,
 * since a device cuts its answer to wLength (2.0 5.2.2); false when none
 * is, or none is well formed (MIN above MAX).
 */
bool uac_volume_range_v2(const uint8_t *bytes, unsigned length,
                         struct uac_volume_range *out);

/*
 * ---- **Kosmos's master onto the control** ----
 *
 * The master is 0 to 256 (`audioproto.h`), and what the audio server does
 * with it today is multiply each sample by master / 256
 * (`user/servers/audio.c`, the mix: gain x master / 256): linear in
 * amplitude. So a level is the same loudness on the speaker's control when
 * the control is set 20 log10(level / 256) dB below its top - -6.02 dB at
 * 128, -24.08 dB at 16, -48.16 dB at 1 - and that is the curve. It is a
 * table of 256 numbers in 1/256 dB, worked out once on the host
 * (`round(5120 x log10(level / 256))`) and held to it by the host test,
 * because the driver has no floating point to take a logarithm with.
 *
 * `top` is the value the master's 256 is: the control's unity, 0 dB,
 * clamped into the range (`uac_volume_unity`), which passes samples
 * untouched - and on QEMU's speaker its MAX, since QEMU scales by its own
 * volume over 255 and only its top is 255 (`xhci.c`'s `speaker_full`).
 */
int16_t uac_volume_unity(const struct uac_volume_range *r);

/*
 * The control's value for `level` (0..256; more is 256): `top` for 256,
 * exactly, as long as it is inside MIN..MAX - unity is not moved onto a
 * step that misses it, since the control rounds to its own nearest
 * (1.0 5.2.2.4.3.2, 2.0 5.2.2); otherwise `top` plus the curve, clamped to MIN..MAX and to
 * `top`, then on the control's own steps - MIN plus a whole number of RES,
 * the nearest, and never above MAX or `top`. 0 is MIN.
 */
int16_t uac_volume_value(const struct uac_volume_range *r, int16_t top,
                         unsigned level);

/*
 * And back, for a person to read: the level whose place on the curve,
 * below `top`, is nearest `value` - before clamping, so a speaker whose
 * MIN is -20 dB reads 25 at its MIN rather than 1. Silence (0x8000) is 0;
 * anything quieter than level 1 and not silence is 1; at or above `top`
 * is 256.
 */
unsigned uac_volume_level(int16_t top, int16_t value);

/*
 * **What to send for a level, and a muted master** - `level` 0 or `muted`
 * is silence:
 *   - where the unit has a mute: `mute` true, and `value` the level's
 *     value still, so unmuting is one request;
 *   - where it has none: on UAC2 `value` is 0x8000, silence, which 2.0
 *     5.2.5.7.2 makes every volume control take; on UAC1 0x8000 too, which
 *     1.0 5.2.2.4.3.2 defines and does not require, so `silent` is false
 *     there and the audio server must send silence itself.
 * `silent` true is the speaker's control silencing it, by itself.
 */
struct uac_volume_set {
    bool    mute;
    int16_t value;
    bool    silent;
};

void uac_volume_for(const struct uac_device *d, const struct uac_volume_range *r,
                    int16_t top, unsigned level, bool muted,
                    struct uac_volume_set *out);

#endif /* KOSMOS_DRIVERS_USB_UAC_DECODE_H */
