/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * /Devices/audio: the one process that may make a noise, so that several can.
 *
 * **The first server in C**, and the reason is the deadline. The device
 * wants a period every 5.8 milliseconds; a garbage collector in this
 * process would arrive whenever it chose, and `gc_pause_max` is 1.25 ms.
 * A collector and a deadline cannot share a process, and the Lua version of
 * this server met its deadline only because a refactor happened to have
 * removed every allocation from the loop - with nothing to stop the next
 * `print` or table literal putting them back, silently.
 *
 * `SPAWN_AUDIO` grants the device to exactly one process, which is what
 * makes per-application volume possible rather than coincidental: if every
 * program could write to the device, the last writer would win and there
 * would be nothing to turn down.
 *
 * **Samples do not arrive here.** Each client creates a ring in its own
 * memory and hands over a capability at `open`; after that it writes
 * periods and says nothing at all. `audioring.h` has the reasoning, and
 * `CLAUDE.md` has the rule: control by message, data by shared memory.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "kosmos.h"
#include "audioring.h"
#include "audioproto.h"
#include "pcmconvert.h"
#include "syscall.h"
#include "usbaudioring.h"

#define STREAM_MAX  8u          /* AUDIO_LIST_MAX, so `streams` shows them all */

struct stream {
    bool               open;
    uint32_t           id;
    long               cap;             /* the client's ring, as we name it */
    struct audio_ring *ring;
    int32_t            gain;            /* 0..256, 256 unity */
    int32_t            balance;         /* -100 .. +100 */
    bool               muted;
    uint32_t           peak;            /* from the last mix, before gain */
    uint64_t           consumed;        /* frames taken from this ring, ever */
    char               name[AUDIO_NAME_MAX];
};

static struct stream streams[STREAM_MAX];
static uint32_t      next_id = 1;
static int32_t       master  = 256;

/*
 * **Muted is not a level.** The volume keys' mute silences everything and
 * keeps `master` as it was, so unmuting comes back to it - and so everything
 * that reads the level, the Mixer's master and the level bar, still reads
 * the level rather than a zero that means two things.
 */
static bool          master_muted = false;

static unsigned      period_bytes;
static unsigned      period_frames;     /* that period, in the position's unit */
static unsigned      device_depth;      /* what the device will hold */

/*
 * **All of it is kept in it, and how often it was found empty is counted.**
 * 4i step e tried the Synth Kit's rule here too (`depth.h`): start full, one
 * period fewer on evidence, one more for good on a dry run. Finding a
 * queue's floor that way costs a gap the first time it is taken faster than
 * a quiet window showed - on the kit's ring that is Groove's own sound, and
 * on the device it is every program's. Under QEMU's HD Audio it was two
 * films in three with a gap in them however steady the evidence asked for,
 * and none in five held at four (`testing.md` 18.276). So the device stays
 * at what its driver holds; `device_dry` - looks, once a stream has filled
 * it, that found it empty - is what `groove --report` says, and what the
 * M700 will be read by.
 */
static bool          device_primed;
static uint32_t      device_dry;
static unsigned long counter_hz;

/* Diagnostics, reported with `streams`. `starved` is the device having room
 * and every ring being empty - the clients are behind. `late` is the worst
 * gap between two turns of this loop - this server is behind. They are the
 * two halves of a missed deadline and nothing used to measure either. */
static uint32_t      mixes, starved, late;

/*
 * **A USB speaker, the other place the mix can go** (`docs/usbaudio.md`
 * U3, `usbaudioring.h`). init hands this server and the USB driver one
 * region; while a speaker is present in it, and it is the one chosen
 * (`chosen`, below), each period mixed here is
 * converted to the speaker's rate, size and channels - `pcm_convert_to`,
 * the resampler `sys.pcm` and the Playback Kit share - and written into the
 * ring rather than to the machine's own device. The speaker's clock drains
 * it; the driver's pulse says when there is room. When the speaker goes,
 * the next period goes to the machine's own device again.
 */
static struct usb_audio_ring *usb;
static bool          usb_active;
static uint32_t      usb_generation;
static uint32_t      usb_keep;          /* frames of the speaker's to hold */
static uint32_t      mix_rate;          /* the mix's: the device's, 44.1 kHz */
static double        usb_phase;
static unsigned char usb_carry[(HAL_SND_PERIOD_BYTES_MAX / 4u + 2u) * 4u];
static size_t        usb_have;          /* bytes of the mix not yet converted */
static uint32_t      usb_periods;       /* written into the ring, ever */

/*
 * **Which device plays** (`docs/usbaudio.md` U4): a name, "" for the
 * newest - a speaker while one is plugged in, the machine's own otherwise.
 * A name is that device whenever it is present and the machine's while it
 * is not, so a chosen speaker plugged in again has the sound back by
 * itself (decision 3). `opt/kosmos/audio=machine` starts with the
 * machine's; the person's choice is kept by `audio.lua` and sent again by
 * the Deskbar, since this server has no namespace to keep it in.
 */
static char          chosen[AUDIO_DEVICE_NAME_MAX];

/*
 * **Who is told when that changes** (AUDIO_OP_WATCH): a capability and the
 * code to pulse it with, as a folder's watch is. Grown as asked; a door
 * whose pulse fails is let go.
 */
struct watcher {
    long     cap;
    uint32_t code;
};

static struct watcher *watchers;
static unsigned      watchers_count, watchers_room;
static bool          tell_pending;
static uint32_t      told_generation;
static int           told_present = -1, told_active = -1;

static struct stream *find(uint32_t id)
{
    unsigned i;

    for (i = 0; i < STREAM_MAX; i++) {
        if (streams[i].open && streams[i].id == id) {
            return &streams[i];
        }
    }

    return NULL;
}

/*
 * One period out of the device, summed from whatever the rings hold.
 *
 * A stream whose ring is empty is skipped rather than waited for: one
 * client falling behind must not take the others down with it. Returns
 * false when nothing was mixed, and the caller must believe it - the Lua
 * version returned "worked" from a count that included empty streams, so it
 * never blocked, span at the top priority band, and took the desktop with
 * it.
 */
/*
 * One period summed from whatever the rings hold, into `out`, as 16-bit
 * stereo at the mix's rate: `period_bytes` of it. False when no ring had
 * anything. Wherever it is going - the machine's own device or a USB
 * speaker - this is the one mix.
 */
static bool mix(int16_t *out)
{
    static int32_t acc[HAL_SND_PERIOD_BYTES_MAX / 2];

    unsigned samples = period_bytes / 2;
    unsigned mixed = 0;
    unsigned i, k;

    if (samples > sizeof(acc) / sizeof(acc[0])) {
        samples = (unsigned)(sizeof(acc) / sizeof(acc[0]));
    }

    memset(acc, 0, samples * sizeof(acc[0]));

    for (i = 0; i < STREAM_MAX; i++) {
        struct stream *s = &streams[i];
        const int16_t *in;
        int32_t g, left, right, peak = 0;

        if (!s->open || !audio_ring_valid(s->ring)) {
            continue;
        }

        /* Acquire: having seen this index, the samples behind it are there. */
        if (audio_ring_acquire(&s->ring->write) - s->ring->read == 0) {
            s->peak = 0;
            continue;
        }

        /*
         * Balance is a pan applied to the gain rather than beside it: left
         * of centre attenuates the right channel and vice versa. That is
         * what a balance control does and is not what a *pan* law does - a
         * real pan holds the total power constant and needs a square root.
         * This is the simple one, and saying so beats implying the other.
         */
        g = (s->muted || master_muted) ? 0 : ((s->gain * master) / 256);
        left = right = g;

        if (s->balance > 0) {
            left = (g * (100 - s->balance)) / 100;
        } else if (s->balance < 0) {
            right = (g * (100 + s->balance)) / 100;
        }

        in = (const int16_t *)(const void *)audio_ring_slot(s->ring,
                                                            s->ring->read);

        for (k = 0; k < samples; k++) {
            int32_t v = in[k];
            int32_t mag = (v < 0) ? -v : v;

            if (mag > peak) {
                peak = mag;
            }

            /* Even samples are the left channel, odd the right: that is what
             * interleaved stereo means and the only place a balance fits. */
            acc[k] += (v * ((k & 1u) ? right : left)) >> 8;
        }

        s->peak = (uint32_t)peak;
        audio_ring_consumed(s->ring, s->ring->read + 1);

        /* Taken, which is not yet heard: the device is holding some of what
         * was taken before this. `publish_positions` is where the two are
         * subtracted, and it is the only place that knows both. */
        s->consumed += period_frames;
        mixed++;
    }

    if (mixed == 0) {
        return false;
    }

    /*
     * Clipped rather than wrapped. Two streams at full scale sum past what
     * sixteen bits hold, and wrapping turns a peak into the opposite sign -
     * the worst noise a mixer can make.
     */
    for (k = 0; k < samples; k++) {
        int32_t v = acc[k];

        if (v > 32767)  { v = 32767; }
        if (v < -32768) { v = -32768; }

        out[k] = (int16_t)v;
    }

    mixes++;
    return true;
}

static bool refill(void)
{
    static int16_t out[HAL_SND_PERIOD_BYTES_MAX / 2];
    unsigned queued;

    if (period_bytes == 0) {
        return false;
    }

    queued = (unsigned)kosmos_snd_queued();

    /* Filled once before it counts: a stream starts with an empty device,
     * and that is not the device running dry. */
    if (device_primed) {
        if (queued == 0) {
            device_dry++;
        }
    } else if (queued >= device_depth) {
        device_primed = true;
    }

    if (queued >= device_depth) {
        return false;                   /* the device has all it is to hold */
    }

    if (!mix(out)) {
        return false;
    }

    kosmos_snd_write(out, (unsigned long)period_bytes);
    return true;
}

/*
 * Whether the mix goes to a USB speaker now, and the ring taken up afresh
 * when it is a speaker not seen before: the conversion's phase and what it
 * carried belong to the last one.
 */
static bool usb_present(void)
{
    bool here = usb != NULL && usb->magic == USB_AUDIO_MAGIC && usb->present != 0;

    RING_BARRIER();
    return here;
}

static bool machine_present(void)
{
    return period_bytes != 0;
}

static bool chosen_is(const char *name)
{
    return strncmp(chosen, name, AUDIO_DEVICE_NAME_MAX) == 0;
}

/* The speaker plays when it is chosen, when nothing is, or when there is
 * no other device to play on. */
static bool usb_wanted(void)
{
    if (!usb_present()) {
        return false;
    }

    return chosen[0] == 0 || !machine_present() || chosen_is(usb->name);
}

static void usb_check(void)
{
    bool here = usb_wanted();
    uint32_t generation;

    RING_BARRIER();

    if (!here) {
        usb_active = false;
        return;
    }

    generation = usb->generation;

    if (!usb_active || generation != usb_generation) {
        usb_generation = generation;
        usb_phase = 0;
        usb_have = 0;
        usb_keep = usb->rate / 1000u * USB_AUDIO_KEEP_MS;

        if (usb_keep > usb->frames / 2u) {
            usb_keep = usb->frames / 2u;
        }
    }

    usb_active = usb->rate != 0 && usb->frames != 0
                 && usb->subframe >= 2 && usb->subframe <= 4
                 && usb->channels != 0;
}

/*
 * One period into the USB speaker's ring, converted, while it holds less
 * than `usb_keep` of the speaker's time. False when it has enough or there
 * was nothing to mix - as `refill`, so the loop sleeps.
 */
static bool refill_usb(void)
{
    static int16_t out[HAL_SND_PERIOD_BYTES_MAX / 2];
    static int16_t converted[HAL_SND_PERIOD_BYTES_MAX];  /* to 2x the rate */
    uint64_t write = usb->write;
    uint64_t waiting = write - usb_audio_load(&usb->read);
    unsigned frame_bytes = usb_audio_frame_bytes(usb);
    size_t used = 0;
    long n, i;

    if (waiting >= usb_keep) {
        return false;
    }

    if (period_bytes == 0 || !mix(out)) {
        return false;
    }

    if (usb_have + period_bytes > sizeof(usb_carry)) {
        usb_have = 0;                   /* cannot happen: one frame carries */
    }

    memcpy(usb_carry + usb_have, out, period_bytes);
    usb_have += period_bytes;

    n = pcm_convert_to(usb_carry, usb_have, (long)mix_rate, 2, 16, 0,
                       &usb_phase, converted, (long)sizeof(converted), &used,
                       (long)usb->rate);
    memmove(usb_carry, usb_carry + used, usb_have - used);
    usb_have -= used;

    for (i = 0; i < n && waiting + (uint64_t)i < usb->frames; i++) {
        usb_audio_put(usb_audio_at(usb, write + (uint64_t)i),
                      converted[2 * i], converted[2 * i + 1],
                      usb->channels, usb->subframe);
    }

    (void)frame_bytes;
    usb_audio_store(&usb->write, write + (uint64_t)i);
    usb_periods++;
    return true;
}

/*
 * Tell each client where its stream has actually got to.
 *
 * **The device queue is the whole difficulty**, and it is why this cannot
 * live on the client's side of the region. A client knows what it wrote and
 * the ring says what was taken, but between "taken" and "heard" sit up to
 * `device_depth` periods that only this process can see. Without this, a
 * position is wrong by the depth of the device - about 23 ms - which is
 * small enough to look right and large enough to be useless for the two
 * things a position is for.
 *
 * Every open stream, every turn of the loop. Eight streams and two stores
 * each against a mixing pass that touches a thousand samples: this is not
 * where the time goes, and a position that is fresh only when somebody asks
 * for it would need somebody to ask.
 */
static void publish_positions(void)
{
    unsigned held = (unsigned)kosmos_snd_queued() * period_frames;
    unsigned i;

    /* A USB speaker: what is in its ring and on the controller's, back in
     * the mix's frames, and what waits to be converted. */
    if (usb_active) {
        uint64_t ahead = usb->write - usb_audio_load(&usb->played);

        held = (unsigned)(ahead * mix_rate / usb->rate) + (unsigned)(usb_have / 4u);
    }

    for (i = 0; i < STREAM_MAX; i++) {
        struct stream *s = &streams[i];

        if (!s->open || !audio_ring_valid(s->ring)) {
            continue;
        }

        audio_ring_position(s->ring,
                            audio_frames_played(s->consumed, (uint64_t)held));
    }
}

static void do_open(const struct audio_request *req, long cap,
                    struct audio_reply *rep)
{
    unsigned i;
    long at;

    if (period_bytes == 0) {
        rep->error = AUDIO_ERR_NO_DEVICE;
        return;
    }

    if (cap < 0) {
        rep->error = AUDIO_ERR_NO_RING;      /* control, but no data path */
        return;
    }

    for (i = 0; i < STREAM_MAX; i++) {
        if (!streams[i].open) {
            break;
        }
    }

    if (i == STREAM_MAX) {
        rep->error = AUDIO_ERR_FULL;
        return;
    }

    at = kosmos_mem_map(cap);

    if (at < 0 || !audio_ring_valid((struct audio_ring *)(uintptr_t)at)) {
        rep->error = AUDIO_ERR_NO_RING;
        return;
    }

    streams[i].open = true;
    streams[i].id = next_id++;
    streams[i].cap = cap;
    streams[i].ring = (struct audio_ring *)(uintptr_t)at;
    streams[i].gain = 256;
    streams[i].balance = 0;
    streams[i].muted = false;
    streams[i].peak = 0;
    streams[i].consumed = 0;

    /* Zeroed here as well as in the client's `ring_create`, because a slot
     * that has been used before is carrying the last stream's position and
     * the client would read it before the first period was ever mixed. */
    audio_ring_position(streams[i].ring, 0);

    /* The name is the client's own and nothing checks it: two copies of one
     * program get two streams called the same thing, which is right - they
     * are two things making a noise and each wants its own fader. */
    memcpy(streams[i].name, req->name, AUDIO_NAME_MAX);
    streams[i].name[AUDIO_NAME_MAX - 1] = '\0';

    rep->error = AUDIO_OK;
    rep->stream = streams[i].id;
    rep->period = period_bytes;
    rep->periods = device_depth;
}

static void do_close(const struct audio_request *req, struct audio_reply *rep)
{
    struct stream *s = find(req->stream);

    if (s == NULL) {
        rep->error = AUDIO_ERR_NO_STREAM;
        return;
    }

    /*
     * The ring was the client's memory and this process only borrowed a view
     * of it. Unmapped before the capability is dropped, because after the
     * drop this process may have no right to name those pages - and dropping
     * first would leave a window where they could be freed underneath a
     * mapping still held here.
     */
    if (s->ring != NULL) {
        unsigned long bytes = AUDIO_RING_DATA
                              + (unsigned long)s->ring->periods
                                * s->ring->period_bytes;

        kosmos_share_unmap((unsigned long)(uintptr_t)s->ring,
                           (bytes + 4095UL) / 4096UL);
    }

    kosmos_cap_drop(s->cap);
    memset(s, 0, sizeof(*s));
    rep->error = AUDIO_OK;
}

static void do_set(const struct audio_request *req, struct audio_reply *rep)
{
    struct stream *s;

    /* Master first, and on its own: a request that only moves the master
     * names no stream and must not be refused for it. */
    if (req->master_muted >= 0) {
        master_muted = (req->master_muted != 0);
    }

    if (req->master >= 0) {
        master = (req->master > 256) ? 256 : req->master;
    }

    if (req->stream == 0) {
        rep->error = AUDIO_OK;
        return;
    }

    s = find(req->stream);

    if (s == NULL) {
        rep->error = AUDIO_ERR_NO_STREAM;
        return;
    }

    if (req->gain >= 0)    { s->gain = (req->gain > 256) ? 256 : req->gain; }
    if (req->balance >= -100 && req->balance <= 100) { s->balance = req->balance; }
    if (req->muted >= 0)   { s->muted = (req->muted != 0); }

    rep->error = AUDIO_OK;
}

static void do_streams(struct audio_reply *rep)
{
    unsigned i;

    rep->error = AUDIO_OK;
    rep->master = (uint32_t)master;
    rep->master_muted = master_muted ? 1u : 0u;
    rep->period = period_bytes;
    rep->periods = device_depth;
    rep->mixes = mixes;
    rep->starved = starved;
    rep->late = late;
    rep->kept = device_depth;
    rep->device_dry = device_dry;
    rep->count = 0;

    for (i = 0; i < STREAM_MAX && rep->count < AUDIO_LIST_MAX; i++) {
        struct stream *s = &streams[i];
        struct audio_stream_info *o;

        if (!s->open) {
            continue;
        }

        o = &rep->list[rep->count++];
        o->stream = s->id;
        o->gain = (uint32_t)s->gain;
        o->balance = s->balance;
        o->muted = s->muted ? 1u : 0u;
        o->peak = s->peak;
        o->queued = audio_ring_valid(s->ring) ? audio_ring_ready(s->ring) : 0u;
        memcpy(o->name, s->name, AUDIO_NAME_MAX);
    }
}

/*
 * **Every output, and the one playing** (U4): the machine's own first, as
 * Mixer lists it, then a speaker in the ring.
 */
static void do_devices(struct audio_reply *rep)
{
    struct audio_device_info *d;

    rep->error = AUDIO_OK;
    rep->devices_count = 0;
    memcpy(rep->chosen, chosen, sizeof(rep->chosen));

    if (machine_present()) {
        d = &rep->devices[rep->devices_count++];
        d->id = AUDIO_DEVICE_MACHINE;
        d->playing = usb_active ? 0u : 1u;
        d->rate = mix_rate;
        d->bits = 16;
        d->channels = period_frames != 0 ? period_bytes / (2u * period_frames) : 2u;
        d->bits_offered = AUDIO_BITS_16;
        d->rates[0] = mix_rate;
        memcpy(d->name, AUDIO_MACHINE_NAME, sizeof(AUDIO_MACHINE_NAME));
    }

    if (usb_present()) {
        unsigned i;

        d = &rep->devices[rep->devices_count++];
        d->id = AUDIO_DEVICE_USB;
        d->playing = usb_active ? 1u : 0u;
        d->rate = usb->rate;
        d->bits = usb->subframe * 8u;
        d->channels = usb->channels;
        d->bits_offered = usb->bits_offered;
        d->vendor = usb->vendor;
        d->product = usb->product;

        for (i = 0; i < AUDIO_RATES_MAX && i < USB_AUDIO_RATES; i++) {
            d->rates[i] = usb->rates[i];
        }

        memcpy(d->name, usb->name, sizeof(d->name));
        d->name[sizeof(d->name) - 1] = 0;
    }
}

static void do_choose(const struct audio_request *req, struct audio_reply *rep)
{
    memcpy(chosen, req->device, sizeof(chosen));
    chosen[sizeof(chosen) - 1] = 0;
    tell_pending = true;
    do_devices(rep);
}

static void do_watch(const struct audio_request *req, long cap,
                     struct audio_reply *rep)
{
    if (cap < 0) {
        rep->error = AUDIO_ERR_BAD_OP;
        return;
    }

    if (watchers_count == watchers_room) {
        unsigned room = watchers_room ? watchers_room * 2u : 8u;
        struct watcher *more = realloc(watchers, room * sizeof(*more));

        if (more == NULL) {
            (void)kosmos_cap_drop(cap);
            rep->error = AUDIO_ERR_FULL;
            return;
        }

        watchers = more;
        watchers_room = room;
    }

    watchers[watchers_count].cap = cap;
    watchers[watchers_count].code = req->stream;
    watchers_count++;
    rep->error = AUDIO_OK;
}

static void unwatch_at(unsigned i)
{
    (void)kosmos_cap_drop(watchers[i].cap);
    watchers[i] = watchers[--watchers_count];
}

static void do_unwatch(const struct audio_request *req, struct audio_reply *rep)
{
    unsigned i;

    for (i = 0; i < watchers_count; i++) {
        if (watchers[i].code == req->stream) {
            unwatch_at(i);
            break;
        }
    }

    rep->error = AUDIO_OK;
}

/* Every watcher pulsed when a speaker came or went, or the one playing
 * changed, or a choice was made. */
static void tell(void)
{
    int present = usb_present() ? 1 : 0;
    int active = usb_active ? 1 : 0;
    uint32_t generation = present ? usb->generation : told_generation;
    unsigned i = 0;

    if (!tell_pending && present == told_present && active == told_active
        && generation == told_generation) {
        return;
    }

    tell_pending = false;
    told_present = present;
    told_active = active;
    told_generation = generation;

    while (i < watchers_count) {
        if (kosmos_pulse(watchers[i].cap, watchers[i].code, 0) < 0) {
            unwatch_at(i);
        } else {
            i++;
        }
    }
}

static void answer(const struct message *in, uint64_t sender, long cap)
{
    /* A pulse - the USB driver's "look at the ring", or anybody's - has
     * nobody to answer, and the loop looks at the ring on every turn. */
    if (in->tag == KOSMOS_PULSE_TAG) {
        return;
    }

    struct message out;
    struct audio_reply *rep = (struct audio_reply *)(void *)out.data;
    const struct audio_request *req =
        (const struct audio_request *)(const void *)in->data;

    memset(&out, 0, sizeof(out));
    out.tag = in->tag;
    out.length = (uint32_t)sizeof(*rep);
    out.cap_plus_one = 0;

    if (in->length < sizeof(*req)) {
        rep->error = AUDIO_ERR_BAD_OP;
        (void)kosmos_reply(sender, &out);
        return;
    }

    switch (req->op) {
    case AUDIO_OP_OPEN:    do_open(req, cap, rep); break;
    case AUDIO_OP_CLOSE:   do_close(req, rep);     break;
    case AUDIO_OP_SET:     do_set(req, rep);       break;
    case AUDIO_OP_STREAMS: do_streams(rep);        break;
    case AUDIO_OP_DEVICES: do_devices(rep);        break;
    case AUDIO_OP_CHOOSE:  do_choose(req, rep);    break;
    case AUDIO_OP_WATCH:   do_watch(req, cap, rep); break;
    case AUDIO_OP_UNWATCH: do_unwatch(req, rep);   break;
    default:               rep->error = AUDIO_ERR_BAD_OP; break;
    }

    (void)kosmos_reply(sender, &out);
}

static bool anyone_open(void)
{
    unsigned i;

    for (i = 0; i < STREAM_MAX; i++) {
        if (streams[i].open) {
            return true;
        }
    }

    return false;
}

/*
 * The loop. Never returns.
 *
 * Its shape is the one thing here that took a whole day to get right, so it
 * is worth stating plainly:
 *
 *   - Mix while the device has room and any ring has samples.
 *   - Otherwise wait for a message *or* a tick, whichever comes first.
 *   - Wait for a message alone only when no stream is open at all.
 *
 * The last line is the one that is not obvious. A client with a ring
 * announces nothing - it writes into shared memory and sends no message -
 * so "no ring has samples right now" is true for a moment just after `open`
 * and means nothing. Blocking on that slept through every stream that never
 * spoke. Nothing can begin playing without an `open` first, and that *is* a
 * message, so an idle machine still blocks and an idle desktop is idle.
 */
void audio_server(long endpoint, long usb_ring)
{
    struct sysinfo info;
    unsigned long last_turn = 0;
    unsigned long us;
    unsigned channels;

    /*
     * **In the audio band from its first instruction** (`kernel/sched.h`):
     * init spawns it with the device and the band, and a child given both
     * is started there (`spawn_finish`). It used to ask here, in its first
     * line, which a thread at NORMAL on a busy core may never reach - the
     * Synth Kit's thread did not, under load (`testing.md` 18.268).
     */
    memset(streams, 0, sizeof(streams));
    memset(&info, 0, sizeof(info));
    (void)kosmos_sysinfo(&info);

    period_bytes = info.audio_period;
    device_depth = info.audio_periods;

    /*
     * Sixteen bits a sample is what the device takes and what `refill`
     * assumes two lines into its loop, so the 2 here is that fact rather
     * than a magic number - and the channel count is asked for because
     * `sysinfo` reports it and a constant would be a second answer to a
     * question the machine already answers.
     */
    channels = info.audio_channels ? info.audio_channels : 2u;
    period_frames = period_bytes / (2u * channels);
    counter_hz = info.counter_hz ? info.counter_hz : 62500000UL;
    us = counter_hz / 1000000UL;

    if (us == 0) {
        us = 1;
    }

    mix_rate = info.audio_rate ? info.audio_rate : PCM_DEVICE_RATE;

    {
        char option[8] = { 0 };
        long at = usb_ring < 0 ? -1 : kosmos_mem_map(usb_ring);

        usb = at < 0 ? NULL : (struct usb_audio_ring *)(uintptr_t)at;

        /* The machine's own from the start, where the boot says so. */
        if (kosmos_boot_option("opt/kosmos/audio", option, sizeof(option)) == 7
            && memcmp(option, "machine", 7) == 0) {
            memcpy(chosen, AUDIO_MACHINE_NAME, sizeof(AUDIO_MACHINE_NAME));
        }
    }


    for (;;) {
        struct message msg;
        uint64_t sender = 0;
        long status;
        unsigned long turn = kosmos_ticks();

        /*
         * Only while something is playing. Otherwise the first turn after a
         * stream opens carries the whole idle wait before it - this server
         * blocks until somebody calls `open`, so that gap is however long
         * the machine took to play something, and it read 367 ms for a
         * server that was perfectly healthy.
         */
        if (anyone_open()) {
            if (last_turn != 0) {
                unsigned long gap = (turn - last_turn) / us;

                if (gap > late) {
                    late = (uint32_t)gap;
                }
            }

            last_turn = turn;
        } else {
            last_turn = 0;
            device_primed = false;      /* the next stream fills it again */
        }

        usb_check();
        tell();
        publish_positions();

        if (usb_active ? refill_usb() : refill()) {
            continue;                   /* there may be room for another */
        }

        if (usb_active ? usb->write - usb->read < usb_keep
                       : (unsigned)kosmos_snd_queued() < device_depth) {
            starved++;                  /* room, and nothing to put in it */
        }

        /*
         * A deadline on the receive, not a sleep.
         *
         * A server that sleeps on a timer answers nobody while it sleeps, so
         * every caller pays a tick - which cost the Music window 45 ms per
         * pass for twelve round trips that should have been microseconds.
         * This waits for whichever comes first, and the sound interrupt
         * wakes it early through `process_wake_audio`.
         */
        status = kosmos_receive(endpoint, &msg, &sender, 0,
                               (anyone_open() || kosmos_snd_queued() > 0)
                               ? 1UL : 0UL);

        if (status == 0) {
            long cap = (msg.cap_plus_one > 0)
                       ? (long)msg.cap_plus_one - 1 : -1;

            answer(&msg, sender, cap);
        }
    }
}
