/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_AUDIOPROTO_H
#define KOSMOS_AUDIOPROTO_H

#include <stdint.h>

/*
 * What you may ask the audio server, written down.
 *
 * **The first protocol in Kosmos that is a definition rather than a
 * convention.** Every other server takes a serialised Lua table: any shape
 * of message is accepted, nothing declares what the shapes are, and the
 * only account of an operation is the code that happens to read it. That is
 * comfortable to write and it means a server cannot be written in anything
 * but Lua, because reading the message needs a `lua_State` to unpack into.
 *
 * `CLAUDE.md` now says the layer decides the language and a server is C. So
 * the message has to be something C can read, and this is it: fixed fields,
 * fixed sizes, no decoding step at all. The server casts the bytes and
 * reads them.
 *
 * **What this costs, honestly.** Adding a field means editing this file and
 * recompiling both sides, where a Lua table would have carried it with
 * nobody told. And an error is a number here rather than a sentence, so the
 * message a person eventually reads is composed by whoever renders it
 * rather than by whoever detected it. Both of those are the price of the
 * interface existing at all, and both are worth it: the shapes are now
 * somewhere a reader can find them.
 *
 * The layout is what C gives it and nothing here is packed - every field is
 * naturally aligned on purpose, so that both sides agree without a pragma
 * that one of them might forget. `_Static_assert` on the sizes keeps that
 * honest.
 */

#define AUDIO_OP_OPEN     1u    /* a ring capability travels with this one */
#define AUDIO_OP_CLOSE    2u
#define AUDIO_OP_SET      3u
#define AUDIO_OP_STREAMS  4u

/* Errors are numbers. The words belong to whoever shows them to a person. */
#define AUDIO_OK              0u
#define AUDIO_ERR_NO_DEVICE   1u
#define AUDIO_ERR_NO_STREAM   2u
#define AUDIO_ERR_NO_RING     3u
#define AUDIO_ERR_FULL        4u
#define AUDIO_ERR_BAD_OP      5u

#define AUDIO_NAME_MAX   24u
#define AUDIO_LIST_MAX    8u    /* streams one `streams` reply can carry */

/*
 * `gain`, `balance` and `muted` are signed so that -1 can mean "leave this
 * one alone". A `set` that had to send every field would make the Mixer
 * read the server's state back before changing one fader, and two things
 * that must agree is how this system keeps hurting itself.
 */
struct audio_request {
    uint32_t op;
    uint32_t stream;            /* which stream, for close and set */
    int32_t  gain;              /* 0..256, 256 is unity; -1 leaves it */
    int32_t  balance;           /* -100 left .. +100 right; -101 leaves it */
    int32_t  muted;             /* 0 or 1; -1 leaves it */
    int32_t  master;            /* 0..256; -1 leaves it */
    char     name[AUDIO_NAME_MAX];
};

struct audio_stream_info {
    uint32_t stream;
    uint32_t gain;
    int32_t  balance;
    uint32_t muted;
    uint32_t peak;              /* before gain: who is making a noise */
    uint32_t queued;            /* periods waiting in its ring */
    char     name[AUDIO_NAME_MAX];
};

struct audio_reply {
    uint32_t error;             /* AUDIO_OK, or why not */

    /* open */
    uint32_t stream;
    uint32_t period;            /* bytes in one period */
    uint32_t periods;           /* how many the device holds */

    /* streams */
    uint32_t master;
    uint32_t mixes;             /* periods mixed, ever */
    uint32_t starved;           /* device had room, every ring was empty */
    uint32_t late;              /* worst gap between turns, microseconds */
    uint32_t count;             /* how many entries in `list` are filled */

    struct audio_stream_info list[AUDIO_LIST_MAX];
};

/*
 * A reply has to fit in a message, and `MSG_BYTES` is 2048.
 *
 * Checked rather than assumed: `AUDIO_LIST_MAX` is the field somebody will
 * raise without thinking, and the failure would be a reply silently
 * truncated rather than a build that stops.
 */
_Static_assert(sizeof(struct audio_request) <= 2048,
               "an audio request must fit in one message");
_Static_assert(sizeof(struct audio_reply) <= 2048,
               "an audio reply must fit in one message - lower AUDIO_LIST_MAX");

#endif /* KOSMOS_AUDIOPROTO_H */
