/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Quake III's sound, into the audio server's ring (`docs/quake3e.md` §5),
 * in place of `sdl_snd.c`.
 *
 * **The engine mixes its own sound** into a circular buffer it believes is
 * a sound card's, a little ahead of where the card has got to, and asks the
 * card where that is (`SNDDMA_GetDMAPos`) - the same interface Quake 1's
 * has, and the same answer as `quake_kosmos.c` gives it: each time it asks,
 * what it has painted since the last time goes from that buffer into the
 * audio server's ring (`audio_ring_feed`), as far as `AHEAD` periods ahead
 * of the speaker and no further, and where the "card" has got to is where
 * that left off. The position moves at exactly the speed the speaker
 * drains, which is all a card's position has to do; the engine's mixing,
 * resampling and spatialising are untouched, and no thread is needed.
 *
 * The ring comes from Lua (`q3.sound`, before `q3.start`), which opened the
 * stream with `audio.lua` as Quake 1's does. Without one the game is
 * silent and says so.
 */

#include "client.h"
#include "snd_local.h"

#include <stdint.h>

#include "lua.h"
#include "lauxlib.h"

#include "audioring.h"

#include "q3_kosmos.h"

#define SND_SAMPLES 65536u      /* mono samples, a power of two: 0.68 s at 48 kHz */
#define SND_AHEAD   6u          /* periods kept ahead of the speaker */

static int16_t            snd_buffer[SND_SAMPLES];
static struct audio_ring *snd_ring;
static uint32_t           snd_rate;
static uint32_t           snd_partial;
static uint32_t           snd_pos;     /* in mono samples, as the engine counts */
static qboolean           snd_on;

int q3_lua_sound(lua_State *L)
{
    struct audio_ring *r = (struct audio_ring *)(uintptr_t)luaL_checkinteger(L, 1);
    lua_Integer rate = luaL_checkinteger(L, 2);

    if (!audio_ring_valid(r) || r->period_bytes % 4u != 0 || rate <= 0) {
        return luaL_error(L, "that is not a stereo sound ring");
    }

    snd_ring = r;
    snd_rate = (uint32_t)rate;
    return 0;
}

qboolean SNDDMA_Init(void)
{
    if (snd_ring == NULL || snd_rate == 0) {
        Com_Printf("No sound: Quake III was not given a stream\n");
        return qfalse;
    }

    memset(snd_buffer, 0, sizeof snd_buffer);
    dma.samplebits = 16;
    dma.isfloat = 0;
    dma.channels = 2;
    dma.samples = SND_SAMPLES;
    dma.fullsamples = SND_SAMPLES / 2;
    dma.submission_chunk = 1;
    dma.speed = (int)snd_rate;
    dma.buffer = (byte *)snd_buffer;
    dma.driver = "kosmos";

    snd_pos = 0;
    snd_partial = 0;
    snd_on = qtrue;

    Com_Printf("Kosmos sound: %d Hz, stereo, 16 bits\n", dma.speed);
    return qtrue;
}

int SNDDMA_GetDMAPos(void)
{
    struct audio_ring *r = snd_ring;

    if (!snd_on || r == NULL) {
        return 0;
    }

    uint32_t period = r->period_bytes / 4u;
    uint32_t queued = audio_ring_ready(r) * period + snd_partial;
    uint32_t want = SND_AHEAD * period;
    uint32_t frames = queued < want ? want - queued : 0;
    uint32_t room = audio_ring_room(r, snd_partial);

    if (frames > room) {
        frames = room;
    }

    /* At most twice round: up to the buffer's end, then from its start. */
    while (frames > 0) {
        uint32_t at = snd_pos / 2u;
        uint32_t to_end = SND_SAMPLES / 2u - at;
        uint32_t n = frames < to_end ? frames : to_end;
        uint32_t left = audio_ring_feed(r, &snd_partial, snd_buffer + snd_pos, n);

        n -= left;
        snd_pos = (snd_pos + n * 2u) & (SND_SAMPLES - 1u);
        frames -= n;

        if (left > 0) {
            break;
        }
    }

    return (int)snd_pos;
}

void SNDDMA_Shutdown(void)
{
    snd_on = qfalse;
    dma.buffer = NULL;
}

void SNDDMA_BeginPainting(void)
{
}

void SNDDMA_Submit(void)
{
}
