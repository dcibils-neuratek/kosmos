/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * /dev/audio: the one process that may make a noise, so that several can.
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
#include <string.h>

#include "kosmos.h"
#include "audioring.h"
#include "audioproto.h"
#include "syscall.h"

#define STREAM_MAX  8u          /* one more than AUDIO_LIST_MAX would show */

struct stream {
    bool               open;
    uint32_t           id;
    long               cap;             /* the client's ring, as we name it */
    struct audio_ring *ring;
    int32_t            gain;            /* 0..256, 256 unity */
    int32_t            balance;         /* -100 .. +100 */
    bool               muted;
    uint32_t           peak;            /* from the last mix, before gain */
    char               name[AUDIO_NAME_MAX];
};

static struct stream streams[STREAM_MAX];
static uint32_t      next_id = 1;
static int32_t       master  = 256;

static unsigned      period_bytes;
static unsigned      device_depth;
static unsigned long counter_hz;

/* Diagnostics, reported with `streams`. `starved` is the device having room
 * and every ring being empty - the clients are behind. `late` is the worst
 * gap between two turns of this loop - this server is behind. They are the
 * two halves of a missed deadline and nothing used to measure either. */
static uint32_t      mixes, starved, late;

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
static bool refill(void)
{
    static int32_t acc[HAL_SND_PERIOD_BYTES_MAX / 2];
    static int16_t out[HAL_SND_PERIOD_BYTES_MAX / 2];

    unsigned samples = period_bytes / 2;
    unsigned mixed = 0;
    unsigned i, k;

    if (period_bytes == 0) {
        return false;
    }

    if ((unsigned)kosmos_snd_queued() >= device_depth) {
        return false;                   /* the device has all it will hold */
    }

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
        g = s->muted ? 0 : ((s->gain * master) / 256);
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

    kosmos_snd_write(out, (unsigned long)samples * 2);
    mixes++;
    return true;
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
    rep->period = period_bytes;
    rep->periods = device_depth;
    rep->mixes = mixes;
    rep->starved = starved;
    rep->late = late;
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

static void answer(const struct message *in, uint64_t sender, long cap)
{
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
void audio_server(long endpoint)
{
    struct sysinfo info;
    unsigned long last_turn = 0;
    unsigned long us;

    memset(streams, 0, sizeof(streams));
    memset(&info, 0, sizeof(info));
    (void)kosmos_sysinfo(&info);

    period_bytes = info.audio_period;
    device_depth = info.audio_periods;
    counter_hz = info.counter_hz ? info.counter_hz : 62500000UL;
    us = counter_hz / 1000000UL;

    if (us == 0) {
        us = 1;
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
        }

        if (refill()) {
            continue;                   /* there may be room for another */
        }

        if ((unsigned)kosmos_snd_queued() < device_depth) {
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
