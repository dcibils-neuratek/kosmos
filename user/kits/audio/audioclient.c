/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A sound stream, from C (`audioclient.h`). The wire is `audioproto.h`'s,
 * as `audio.lua` sends it: the client makes the ring and hands the server a
 * capability to it with `open`, and the server can reach that ring and
 * nothing else in this process.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "audioproto.h"
#include "audioclient.h"

static long audio = -1;

void audio_client_start(long audio_cap)
{
    audio = audio_cap;
}

long audio_client_cap(void)
{
    return audio;
}

void audio_format_of(struct audio_format *out)
{
    struct sysinfo info;

    memset(out, 0, sizeof *out);

    if (kosmos_sysinfo(&info) < 0) {
        return;
    }

    out->rate = info.audio_rate;
    out->channels = info.audio_channels;
    out->period = info.audio_period;
    out->periods = info.audio_periods;
}

static unsigned ring_pages(const struct audio_ring *r)
{
    return (AUDIO_RING_DATA + r->periods * r->period_bytes + 4095u) / 4096u;
}

struct audio_ring *audio_ring_create(unsigned period_bytes, unsigned periods, long *cap)
{
    struct audio_ring *r;
    long at;

    if (period_bytes == 0 || period_bytes > HAL_SND_PERIOD_BYTES_MAX) {
        *cap = SYS_ERR_NO_DEVICE;
        return NULL;
    }

    if (periods == 0 || periods > 64) {
        periods = AUDIO_RING_PERIODS;
    }

    *cap = kosmos_mem_create((AUDIO_RING_DATA + periods * period_bytes + 4095u) / 4096u);

    if (*cap < 0) {
        return NULL;
    }

    at = kosmos_mem_map(*cap);

    if (at < 0) {
        (void)kosmos_cap_drop(*cap);
        *cap = at;
        return NULL;
    }

    r = (struct audio_ring *)(uintptr_t)at;
    memset(r, 0, AUDIO_RING_DATA);
    r->periods = periods;
    r->period_bytes = period_bytes;
    r->write = 0;
    r->read = 0;

    /* Last, so that a server which maps this while it is being built cannot
     * mistake a half-made ring for one. */
    audio_ring_publish(r, 0);
    r->magic = AUDIO_RING_MAGIC;
    return r;
}

static long ask(struct audio_request *req, long pass, struct audio_reply *out)
{
    struct message msg, reply;
    long status;

    memset(&msg, 0, sizeof msg);
    memcpy(msg.data, req, sizeof *req);
    msg.length = sizeof *req;
    msg.cap_plus_one = pass >= 0 ? (uint32_t)(pass + 1) : 0u;

    status = kosmos_call(audio, &msg, &reply);

    if (status < 0) {
        return status;
    }

    memset(out, 0, sizeof *out);
    memcpy(out, reply.data, reply.length < sizeof *out ? reply.length : sizeof *out);
    return out->error;
}

long audio_stream_open(struct audio_stream *out, const char *name, unsigned periods)
{
    struct audio_request req;
    struct audio_reply rep;
    long r;

    memset(out, 0, sizeof *out);
    out->cap = -1;
    audio_format_of(&out->format);

    if (audio < 0 || out->format.period == 0) {
        return AUDIO_ERR_NO_DEVICE;
    }

    out->ring = audio_ring_create(out->format.period, periods, &out->cap);

    if (out->ring == NULL) {
        return out->cap;
    }

    memset(&req, 0, sizeof req);
    req.op = AUDIO_OP_OPEN;
    req.gain = -1;
    req.balance = -101;
    req.muted = -1;
    req.master = -1;
    req.master_muted = -1;
    strncpy(req.name, name != NULL ? name : "sound", AUDIO_NAME_MAX - 1);

    r = ask(&req, out->cap, &rep);

    if (r != AUDIO_OK) {
        audio_stream_close(out);
        return r;
    }

    out->id = rep.stream;
    return 0;
}

void audio_stream_close(struct audio_stream *s)
{
    if (s->id != 0 && audio >= 0) {
        struct audio_request req;
        struct audio_reply rep;

        memset(&req, 0, sizeof req);
        req.op = AUDIO_OP_CLOSE;
        req.stream = s->id;
        req.gain = -1;
        req.balance = -101;
        req.muted = -1;
        req.master = -1;
        req.master_muted = -1;
        (void)ask(&req, -1, &rep);
    }

    if (s->ring != NULL) {
        (void)kosmos_share_unmap((unsigned long)(uintptr_t)s->ring, ring_pages(s->ring));
    }

    if (s->cap >= 0) {
        (void)kosmos_cap_drop(s->cap);
    }

    memset(s, 0, sizeof *s);
    s->cap = -1;
}

long audio_stream_set(const struct audio_stream *s, int gain, int muted)
{
    struct audio_request req;
    struct audio_reply rep;

    if (s->id == 0 || audio < 0) {
        return AUDIO_ERR_NO_STREAM;
    }

    memset(&req, 0, sizeof req);
    req.op = AUDIO_OP_SET;
    req.stream = s->id;
    req.gain = gain;
    req.balance = -101;
    req.muted = muted;
    req.master = -1;
    req.master_muted = -1;
    return ask(&req, -1, &rep);
}
