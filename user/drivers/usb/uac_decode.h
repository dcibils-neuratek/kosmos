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

#endif /* KOSMOS_DRIVERS_USB_UAC_DECODE_H */
