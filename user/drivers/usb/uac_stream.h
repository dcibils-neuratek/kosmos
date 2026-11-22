/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DRIVERS_USB_UAC_STREAM_H
#define KOSMOS_DRIVERS_USB_UAC_STREAM_H

/*
 * **A USB speaker's stream, as arithmetic** (`docs/usbaudio.md` U2): how
 * many frames each interval carries, and the samples of a tone laid out as
 * the setting lays them out. No hardware in it, as `uac_decode.c` has none,
 * so `tools/test_uacdecode.c` holds it to a second of every rate.
 *
 * **The frames an interval.** A synchronous or adaptive endpoint is fed at
 * the rate it was set to, measured against the bus's start of frame: a
 * millisecond at full speed, 125 us at high speed, times the endpoint's
 * interval. 48 kHz is 48 frames every millisecond, exactly; 44.1 kHz is 44
 * nine times and 45 the tenth. So the count is kept with its remainder and
 * nothing drifts - over any second the frames are the rate, to the frame.
 * An asynchronous endpoint says its own count on a feedback endpoint; that
 * is U6, and starts from the same nominal count.
 */

#include <stdbool.h>
#include <stdint.h>

struct uac_clock {
    uint32_t rate;              /* frames a second */
    uint32_t uframes;           /* 125 us microframes an interval: 1 to 2^15 */
    uint32_t remainder;         /* rate x uframes not yet carried, of 8000 */
};

void uac_clock_init(struct uac_clock *k, uint32_t rate, uint32_t uframes);

/* The frames the next interval carries. */
uint32_t uac_clock_next(struct uac_clock *k);

/* The most any interval carries at `rate`: what one TD must have room for. */
uint32_t uac_clock_most(uint32_t rate, uint32_t uframes);

/*
 * **A tone**, for `opt/kosmos/usbaudio=tone`: a sine at `hz`, every channel
 * the same, `level` its peak out of 32767. Its phase is a fraction of a
 * turn in 32 bits, carried from one call to the next so intervals join with
 * no click. The sine is Bhaskara's (about 0.2% at worst): integers only.
 */
struct uac_tone {
    uint32_t phase;
    uint32_t step;              /* of a turn, a frame */
    int32_t  level;
};

void uac_tone_init(struct uac_tone *t, uint32_t hz, uint32_t rate,
                   int32_t level);

/* The next sample, -level to level. */
int32_t uac_tone_next(struct uac_tone *t);

/*
 * `frames` of the tone into `out`, as `channels` samples a frame of
 * `subframe` bytes each, little-endian (Frmts10 2.2.5: Type I PCM), the
 * sample's high bits in the high bytes. `subframe` is 2, 3 or 4; anything
 * else writes silence. Returns the bytes written.
 */
uint32_t uac_tone_fill(struct uac_tone *t, uint8_t *out, uint32_t frames,
                       unsigned channels, unsigned subframe);

#endif /* KOSMOS_DRIVERS_USB_UAC_STREAM_H */
