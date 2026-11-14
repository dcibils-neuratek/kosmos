/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A sound stream, from C** (`docs/media.md` M4): what `audio.lua` is to a
 * program in Lua, for code that has no `lua_State` - JetSki's web process,
 * which plays a film's sound. One door, two callers: `sys.ring_create` is
 * `audio_ring_create` underneath, so a ring is built one way only.
 *
 * The capability is the `/Devices/audio` this program was given, handed
 * over once by the runner (`sys.storage_start`, proto "audio") or by the
 * process that started this one (`audio_client_start`). Without one there
 * is no sound, which is a supported way to run.
 */
#ifndef KOSMOS_AUDIOCLIENT_H
#define KOSMOS_AUDIOCLIENT_H

#include <stdint.h>

#include "audioring.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The device's format, from the machine: zeroes when it has no sound. */
struct audio_format {
    uint32_t rate, channels;
    uint32_t period;            /* bytes in one period */
    uint32_t periods;           /* how many the device holds */
};

void audio_format_of(struct audio_format *out);

/*
 * A ring of `periods` periods of `period_bytes`, made and mapped in this
 * process: the address, and its capability in `*cap`, or 0 and the
 * syscall's error in `*cap`. `periods` 0, or above 64, is the default.
 */
struct audio_ring *audio_ring_create(unsigned period_bytes, unsigned periods, long *cap);

/* The `/Devices/audio` to open streams on, or -1 for none. */
void audio_client_start(long audio_cap);
long audio_client_cap(void);

struct audio_stream {
    uint32_t id;
    long cap;                   /* the ring's, ours */
    struct audio_ring *ring;
    struct audio_format format;
};

/*
 * A stream named `name` for the Mixer, its ring `periods` deep. 0, or an
 * AUDIO_ERR_ number, or a negative syscall error.
 */
long audio_stream_open(struct audio_stream *out, const char *name, unsigned periods);
void audio_stream_close(struct audio_stream *s);

/* Its fader at the server: `gain` 0..256 (256 is unity) and `muted` 0 or 1,
 * either -1 to leave it. 0, or an error as `open`'s. */
long audio_stream_set(const struct audio_stream *s, int gain, int muted);

#ifdef __cplusplus
}
#endif

#endif
