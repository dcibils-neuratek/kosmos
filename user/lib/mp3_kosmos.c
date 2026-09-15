/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The MP3 Kit: bytes in, sixteen-bit PCM out.
 *
 * A finished algorithm, which is the case `CLAUDE.md` says belongs in C
 * without a profile to justify it. MP3 is not going to change: there is
 * nothing here to reload, so the cost of C is zero and the speed is free.
 * The same argument put DEFLATE and the PDF scanner in kits.
 *
 * **It decodes and it does not play.** `music` still owns the loop, still
 * reads its own window of the file, still resamples through `sys.pcm` and
 * still hands periods to `/dev/audio`. This replaces exactly one step - the
 * one that turned WAV's bytes into samples by reading them - and everything
 * downstream is the path that already worked.
 *
 * That is why `decode` reports the rate and the channel count rather than
 * converting: an MP3 is 44100 or 48000 or 22050 stereo or mono, the device
 * is what it is, and `sys.pcm` already knows how to get from one to the
 * other. A kit that resampled would be a second answer to a question the
 * system had answered.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

/*
 * Sixteen-bit output, which is the default and is what the device takes.
 * Saying so out loud because `MINIMP3_FLOAT_OUTPUT` is one define away and
 * would silently double every buffer below.
 */
#undef MINIMP3_FLOAT_OUTPUT
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#define MP3_MT  "kosmos.mp3"

struct decoder {
    mp3dec_t     dec;
    /* One frame's worth, which is the most a single call can produce:
     * 1152 samples of two channels. */
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int          rate;
    int          channels;
};

static struct decoder *check(lua_State *L)
{
    return (struct decoder *)luaL_checkudata(L, 1, MP3_MT);
}

static int l_decoder(lua_State *L)
{
    struct decoder *d = (struct decoder *)lua_newuserdatauv(L, sizeof(*d), 0);

    memset(d, 0, sizeof(*d));
    mp3dec_init(&d->dec);

    luaL_getmetatable(L, MP3_MT);
    lua_setmetatable(L, -2);
    return 1;
}

/*
 * Start again, keeping nothing.
 *
 * Needed after a seek and after loading a different file: the decoder holds
 * the previous frame's overlap and the synthesis filter's history, and
 * feeding it bytes from somewhere else without this produces a few
 * milliseconds of the last song at the start of the new one.
 */
static int l_reset(lua_State *L)
{
    struct decoder *d = check(L);

    mp3dec_init(&d->dec);
    d->rate = 0;
    d->channels = 0;
    return 0;
}

/*
 * Decode until there is `want` bytes of PCM or the input runs out.
 *
 * Returns the samples, how many input bytes were used, the rate and the
 * channel count.
 *
 * **A frame at a time is what the decoder offers; a loop is what the caller
 * wants.** `mp3dec_decode_frame` produces 1152 samples - about 26 ms - and
 * the player asks for a period at a time. Looping in here rather than in Lua
 * is the difference between one crossing of the boundary per turn and forty.
 *
 * The zero-sample case is not an error and is the reason this is a loop
 * rather than a single call. A file starts with an ID3 tag, and a stream
 * picked up mid-way starts with the tail of a frame; the decoder consumes
 * those and reports `frame_bytes` with no samples, and the caller must skip
 * them rather than treat them as the end.
 */
static int l_decode(lua_State *L)
{
    struct decoder *d = check(L);
    size_t len = 0;
    const uint8_t *in = (const uint8_t *)luaL_checklstring(L, 2, &len);
    lua_Integer want = luaL_optinteger(L, 3, 4096);
    size_t at = 0;
    luaL_Buffer b;
    size_t produced = 0;

    if (want < 0) {
        want = 0;
    }

    luaL_buffinit(L, &b);

    while (at < len && produced < (size_t)want) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&d->dec, in + at, (int)(len - at),
                                          d->pcm, &info);

        if (info.frame_bytes == 0) {
            /* Not enough bytes left to make a frame out of. The caller keeps
             * what it has and comes back with more, which is exactly what
             * the player's `carry` is for. */
            break;
        }

        at += (size_t)info.frame_bytes;

        if (samples > 0) {
            size_t bytes = (size_t)samples * (size_t)info.channels
                           * sizeof(mp3d_sample_t);

            luaL_addlstring(&b, (const char *)d->pcm, bytes);
            produced += bytes;

            d->rate = info.hz;
            d->channels = info.channels;
        }
    }

    luaL_pushresult(&b);
    lua_pushinteger(L, (lua_Integer)at);
    lua_pushinteger(L, d->rate);
    lua_pushinteger(L, d->channels);
    return 4;
}

/*
 * **A Xing or Info header**, which an encoder writes into a first frame that
 * holds no music: how many frames follow and how many bytes they take.
 *
 * It sits where the side information ends, and how long that is depends on
 * the frame - 32 bytes for MPEG-1 in stereo, 17 in mono, 17 and 9 for MPEG-2
 * and 2.5 - after the four-byte header and a CRC's two when there is one.
 * Those are what `L3_read_side_info` in minimp3 reads; "Xing" then a
 * big-endian flags word, bit 0 the frame count following, bit 1 the byte
 * count after it.
 *
 * **Why it matters**: without it the header frame was taken for the music.
 * Basket Case's is 64 kbps and 208 bytes, and its 7441 frames after it run at
 * 160 to 320 - so Music said `MP3 64 kbps` and `9:58` for a song of 3:14 at
 * 197 on average. Checked against that file on the Mac: 7441 frames and
 * 4786800 bytes, the file less its ID3 tag, give 194.377 s, which is what
 * macOS says too.
 */
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
           | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool vbr_header(const uint8_t *frame, size_t frame_bytes,
                       uint32_t *frames, uint32_t *bytes, bool *variable)
{
    bool mpeg1 = (frame[1] & 0x08u) != 0;
    bool mono = (frame[3] & 0xc0u) == 0xc0u;
    bool crc = (frame[1] & 0x01u) == 0;
    size_t at = 4u + (crc ? 2u : 0u)
                + (mpeg1 ? (mono ? 17u : 32u) : (mono ? 9u : 17u));
    uint32_t flags;

    if (frame_bytes < 8u || at + 8u > frame_bytes
        || (memcmp(frame + at, "Xing", 4) != 0
            && memcmp(frame + at, "Info", 4) != 0)) {
        return false;
    }

    /* "Info" is what LAME writes for a constant bitrate, "Xing" otherwise. */
    *variable = memcmp(frame + at, "Xing", 4) == 0;
    flags = be32(frame + at + 4u);
    at += 8u;
    *frames = 0;
    *bytes = 0;

    if ((flags & 1u) != 0) {
        if (at + 4u > frame_bytes) {
            return false;
        }

        *frames = be32(frame + at);
        at += 4u;
    }

    if ((flags & 2u) != 0 && at + 4u <= frame_bytes) {
        *bytes = be32(frame + at);
    }

    return *frames != 0;
}

/*
 * What this file is, without committing to playing it.
 *
 * The player wants a rate and a channel count before the first period, to
 * put on screen and to set the resampler up with. Decoding one frame is the
 * only way to know: an MP3 has no header, only a sequence of frames, and
 * the first one is the header.
 *
 * A separate decoder rather than the caller's, so probing does not leave
 * the real one holding half a frame of overlap.
 */
static int l_probe(lua_State *L)
{
    size_t len = 0;
    const uint8_t *in = (const uint8_t *)luaL_checklstring(L, 1, &len);
    mp3dec_t dec;
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t at = 0;

    mp3dec_init(&dec);

    while (at < len) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&dec, in + at, (int)(len - at),
                                          pcm, &info);

        if (info.frame_bytes == 0) {
            break;
        }

        at += (size_t)info.frame_bytes;

        if (samples > 0) {
            const uint8_t *frame = in + at - (size_t)info.frame_bytes;
            uint32_t frames = 0, bytes = 0;
            bool variable = false;
            bool counted = vbr_header(frame, (size_t)info.frame_bytes,
                                      &frames, &bytes, &variable);
            lua_Integer kbps = info.bitrate_kbps;

            lua_createtable(L, 0, 9);

            lua_pushinteger(L, info.hz);
            lua_setfield(L, -2, "rate");
            lua_pushinteger(L, info.channels);
            lua_setfield(L, -2, "channels");
            lua_pushinteger(L, 16);
            lua_setfield(L, -2, "bits");
            lua_pushinteger(L, info.layer);
            lua_setfield(L, -2, "layer");

            /*
             * A header that counts the frames gives the length exactly -
             * each frame is `samples` long, 1152 for MPEG-1 - and with the
             * bytes, the average bitrate, which is what a variable file is
             * said to be. The music starts after that frame, not at it.
             */
            if (counted) {
                double seconds = (double)frames * (double)samples
                                 / (double)info.hz;

                lua_pushnumber(L, (lua_Number)seconds);
                lua_setfield(L, -2, "seconds");
                lua_pushinteger(L, (lua_Integer)frames);
                lua_setfield(L, -2, "frames");
                lua_pushboolean(L, variable);
                lua_setfield(L, -2, "vbr");

                if (bytes > 0 && seconds > 0.0) {
                    kbps = (lua_Integer)((double)bytes * 8.0 / seconds
                                         / 1000.0 + 0.5);
                }
            }

            lua_pushinteger(L, kbps);
            lua_setfield(L, -2, "bitrate");

            /* Where the audio starts, so the player can skip an ID3 tag
             * rather than feeding it to the decoder every time it rewinds. */
            lua_pushinteger(L, (lua_Integer)(counted
                                             ? at
                                             : at - (size_t)info.frame_bytes));
            lua_setfield(L, -2, "offset");

            return 1;
        }
    }

    lua_pushnil(L);
    lua_pushliteral(L, "no MP3 frame in the first part of this file");
    return 2;
}

void kosmos_mp3_kit(lua_State *L)
{
    static const luaL_Reg methods[] = {
        { "decode", l_decode },
        { "reset",  l_reset },
        { NULL, NULL }
    };

    static const luaL_Reg api[] = {
        { "decoder", l_decoder },
        { "probe",   l_probe },
        { NULL, NULL }
    };

    if (luaL_newmetatable(L, MP3_MT)) {
        luaL_newlib(L, methods);
        lua_setfield(L, -2, "__index");
    }

    lua_pop(L, 1);

    luaL_newlib(L, api);

    /* What one call can produce, so a caller can size a buffer rather than
     * discover the number. */
    lua_pushinteger(L, MINIMP3_MAX_SAMPLES_PER_FRAME * 2);
    lua_setfield(L, -2, "FRAME_MAX");
}
