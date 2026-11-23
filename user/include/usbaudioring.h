/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_USBAUDIORING_H
#define KOSMOS_USBAUDIORING_H

/*
 * **The mix, on its way to a USB speaker** (`docs/usbaudio.md` U3): a ring
 * of the device's own frames in a region init makes once and hands to two
 * processes - the audio server, which writes the mix into it converted to
 * the speaker's rate and size, and `xhci`, which takes from it a
 * millisecond at a time as the speaker's clock asks. As the game pads'
 * page is (`padring.h`), so nothing about it is set up while sound plays,
 * and no capability is passed at run time that another program could pass.
 *
 * **One producer and one consumer, indices only** (`audioring.h`'s shape,
 * and its barrier): the server alone writes `write` and the data behind it;
 * `xhci` alone writes everything else. They count frames for ever, in 64
 * bits, so the difference is what is waiting and nothing wraps but the
 * place in `data`.
 *
 * **Who is there.** `xhci` says a speaker arrived by setting its format,
 * starting its indices at nought, adding one to `generation` and then
 * setting `present` - each after the last, behind the barrier - and that
 * it left by clearing `present`. The server writes only while `present`
 * holds, and starts from `write` afresh whenever `generation` is not the
 * one it last saw. Then it pulses `/Devices/audio`, which says only
 * "look": a pulse from anybody else is a look that finds nothing new.
 *
 * **When it is woken.** `xhci` pulses the server, `AUDIO_PULSE_USB`, after
 * a pass that took frames - about every 8 ms, as its TDs come back - and
 * when a speaker arrives or leaves. The server fills to `USB_AUDIO_KEEP`
 * of the device's time and sleeps. Neither waits on the other.
 *
 * **Where a stream has got to**: `played` is what the speaker has had, by
 * the TDs that came back; `write - played` is what is between the mix and
 * the ear - this ring and the TDs on the controller's.
 *
 * **And who the speaker is** (U4): its product string, its vendor and
 * product numbers, and what its settings offer - written by `xhci` with the
 * format, before `present` - so the server lists it for Mixer, and keeps a
 * choice by its name, from this region alone.
 */

#include <stdint.h>

#include "audioring.h"

#define USB_AUDIO_MAGIC         0x55414331u     /* "UAC1" */
#define USB_AUDIO_RING_PAGES    9u
#define USB_AUDIO_RING_HEADER   128u
#define USB_AUDIO_RING_BYTES    (USB_AUDIO_RING_PAGES * 4096u \
                                 - USB_AUDIO_RING_HEADER)

#define USB_AUDIO_RATES         6u
#define USB_AUDIO_NAME          32u

/* The pulse `xhci` sends `/Devices/audio`: look at the ring. */
#define AUDIO_PULSE_USB         0x55534231u     /* "USB1" */

/* What the server keeps in the ring, in milliseconds of the device's time:
 * four of HD Audio's periods, as decision 2 has it. */
#define USB_AUDIO_KEEP_MS       23u

struct usb_audio_ring {
    uint32_t magic;             /* set once, by xhci, when it maps it */
    uint32_t generation;        /* one more each time a speaker arrives */
    uint32_t present;           /* 1 while a speaker takes from it */
    uint32_t rate;              /* the speaker's: frames a second */
    uint32_t channels;
    uint32_t subframe;          /* bytes a sample: 2, 3 or 4 */
    uint32_t frames;            /* how many fit in `data` */
    uint32_t starved;           /* intervals xhci found short, since it came */
    uint64_t write;             /* the server's: frames written, ever */
    uint64_t read;              /* xhci's: frames taken into TDs */
    uint64_t played;            /* xhci's: frames the speaker has had */
    uint32_t vendor, product;   /* USB's numbers */
    uint32_t bits_offered;      /* AUDIO_BITS_* of audioproto.h */
    uint32_t rates[USB_AUDIO_RATES];  /* offered, 0 past the last */
    char     name[USB_AUDIO_NAME];    /* its product string, ended */
    uint8_t  pad[USB_AUDIO_RING_HEADER - 124];
    uint8_t  data[USB_AUDIO_RING_BYTES];
};

_Static_assert(__builtin_offsetof(struct usb_audio_ring, pad) == 124,
               "the USB audio ring's header is as written");
_Static_assert(sizeof(struct usb_audio_ring) == USB_AUDIO_RING_PAGES * 4096u,
               "the USB audio ring is its pages exactly");

static inline unsigned usb_audio_frame_bytes(const struct usb_audio_ring *r)
{
    return r->channels * r->subframe;
}

/* Where frame `index` lives in `data`. */
static inline uint8_t *usb_audio_at(struct usb_audio_ring *r, uint64_t index)
{
    return r->data + (index % r->frames) * usb_audio_frame_bytes(r);
}

static inline uint64_t usb_audio_load(const volatile uint64_t *index)
{
    uint64_t v = *index;

    RING_BARRIER();
    return v;
}

static inline void usb_audio_store(volatile uint64_t *index, uint64_t to)
{
    RING_BARRIER();
    *index = to;
}

/*
 * A stereo 16-bit frame, the mix's, into the device's frame at `out`: the
 * sample widened into the high bytes of a larger subframe; two channels
 * copied into the first two of more, the rest silent; one channel the
 * average of two.
 */
static inline void usb_audio_put(uint8_t *out, int16_t left, int16_t right,
                                 unsigned channels, unsigned subframe)
{
    unsigned ch, b;

    for (ch = 0; ch < channels; ch++) {
        int32_t v = channels == 1 ? ((int32_t)left + right) / 2
                  : ch == 0 ? left : ch == 1 ? right : 0;
        uint32_t s = (uint32_t)v << ((subframe - 2u) * 8u);

        for (b = 0; b < subframe; b++) {
            *out++ = (uint8_t)(s >> (8u * b));
        }
    }
}

#endif /* KOSMOS_USBAUDIORING_H */
