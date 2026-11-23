/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Media Kit's Lua door** (`docs/media.md` M6a), `/Kosmos/Kits/media`:
 * in no image but `media.elf`, so `use("/Kosmos/Kits/media")` finds it only
 * in a program that runs there - Video, `listen`, and the media family
 * after them - as the WebKit Kit is found only in `webkit.elf`.
 *
 *   media.version()            FFmpeg's, as `media_version` says it
 *   media.open(path)           a source, or nil and why
 *   src:streams()              { { kind, codec, duration_us, width, height,
 *                                  bits, fps_num, fps_den, rate, channels } }
 *   src:read()                 the next packet, or nil at the end, or nil
 *                              and why when the reading stopped
 *   src:seek(us)               to the key picture at or before `us`
 *   src:decoder(i [, threads]) a decoder for stream `i` (M6b, below)
 *   src:close()
 *   packet.stream, .pts_us, .dts_us, .duration_us, .key, .size
 *
 * **The bytes stay in C.** A packet is a userdata over the kit's own
 * packet, freed when it is collected or `free`d, and nothing of what it
 * holds becomes a Lua string (`CLAUDE.md`, *Pixels never go inside a Lua
 * table*, and the same for every byte a film is made of): Lua says which
 * packet goes to which decoder, and C does everything to its bytes.
 *
 * **The file comes through the Storage Kit**, as every C program's does:
 * the path is resolved against the mount table the runner handed this
 * process, so a source reaches exactly what its program's `fs` does, and
 * nothing more. No POSIX in this file: it is Kosmos's own kit, and the
 * POSIX layer is the ported FFmpeg's under it (`CLAUDE.md`, *Compatibility
 * inside a process*).
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "gfx_draw.h"
#include "media.h"
#include "storage.h"

#define SOURCE_MT "kosmos.media.source"
#define PACKET_MT "kosmos.media.packet"

struct source {
    struct media_source *src;
    struct media_io io;
    char path[STORAGE_PATH_MAX];
    uint64_t at;                        /* where the next read begins */
    uint64_t size;
};

struct packet {
    struct media_packet p;
    int held;                           /* 1 while `p` is the kit's to free */
};

/* The Storage Kit as the Media Kit's reader. */
static long file_read(void *ctx, uint8_t *buf, size_t n)
{
    struct source *s = ctx;
    long got;

    if (s->at >= s->size) {
        return 0;
    }

    got = storage_read(s->path, s->at, buf, n);

    if (got > 0) {
        s->at += (uint64_t)got;
    }

    return got;
}

static int64_t file_seek(void *ctx, int64_t offset, int whence)
{
    struct source *s = ctx;
    int64_t to;

    switch (whence) {
    case 0:  to = offset; break;
    case 1:  to = (int64_t)s->at + offset; break;
    case 2:  to = (int64_t)s->size + offset; break;
    default: return -1;
    }

    if (to < 0) {
        return -1;
    }

    s->at = (uint64_t)to;
    return to;
}

static struct source *check_source(lua_State *L)
{
    struct source *s = (struct source *)luaL_checkudata(L, 1, SOURCE_MT);

    if (s->src == NULL) {
        luaL_error(L, "this film is closed");
    }

    return s;
}

static int l_version(lua_State *L)
{
    lua_pushstring(L, media_version());
    return 1;
}

static int l_open(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    struct storage_stat st;
    struct source *s;
    char why[160] = { 0 };
    int r;

    s = (struct source *)lua_newuserdatauv(L, sizeof *s, 0);
    memset(s, 0, sizeof *s);
    luaL_setmetatable(L, SOURCE_MT);

    if (storage_absolute(path, s->path) != STORAGE_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "the path is too long");
        return 2;
    }

    r = storage_stat(s->path, &st);

    if (r != STORAGE_OK || st.kind != STORAGE_KIND_FILE) {
        lua_pushnil(L);
        lua_pushfstring(L, "there is no file at %s", path);
        return 2;
    }

    s->size = st.size;
    s->io.ctx = s;
    s->io.read = file_read;
    s->io.seek = file_seek;
    s->io.size = (int64_t)st.size;
    s->src = media_open(&s->io, why, sizeof why);

    if (s->src == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, why[0] ? why : "it is not a film or a song this kit reads");
        return 2;
    }

    return 1;
}

static const char *kind_name(int kind)
{
    return kind == MEDIA_VIDEO ? "video" : kind == MEDIA_AUDIO ? "audio" : "other";
}

static void set_int(lua_State *L, const char *name, lua_Integer v)
{
    lua_pushinteger(L, v);
    lua_setfield(L, -2, name);
}

static int l_streams(lua_State *L)
{
    struct source *s = check_source(L);
    int n = media_stream_count(s->src);
    int i;

    lua_createtable(L, n > 0 ? n : 0, 0);

    for (i = 0; i < n; i++) {
        struct media_stream m;

        if (media_stream(s->src, i, &m) != 0) {
            continue;
        }

        lua_createtable(L, 0, 10);
        lua_pushstring(L, kind_name(m.kind));
        lua_setfield(L, -2, "kind");
        lua_pushstring(L, m.codec);
        lua_setfield(L, -2, "codec");

        if (m.duration_us != MEDIA_NO_TIME) {
            set_int(L, "duration_us", m.duration_us);
        }

        if (m.kind == MEDIA_VIDEO) {
            set_int(L, "width", m.width);
            set_int(L, "height", m.height);
            set_int(L, "bits", m.bits);
            set_int(L, "fps_num", m.fps_num);
            set_int(L, "fps_den", m.fps_den);
        } else if (m.kind == MEDIA_AUDIO) {
            set_int(L, "rate", m.rate);
            set_int(L, "channels", m.channels);
        }

        lua_rawseti(L, -2, i + 1);
    }

    return 1;
}

static int l_read(lua_State *L)
{
    struct source *s = check_source(L);
    struct packet *p;
    char why[160];
    int r;

    p = (struct packet *)lua_newuserdatauv(L, sizeof *p, 0);
    memset(p, 0, sizeof *p);
    luaL_setmetatable(L, PACKET_MT);
    r = media_read(s->src, &p->p);

    if (r == 1) {
        p->held = 1;
        return 1;
    }

    lua_pushnil(L);

    if (r == 0) {
        return 1;                       /* the end */
    }

    lua_pushstring(L, media_error(r, why, sizeof why));
    return 2;
}

static int l_seek(lua_State *L)
{
    struct source *s = check_source(L);
    int r = media_seek(s->src, (int64_t)luaL_checkinteger(L, 2));
    char why[160];

    if (r < 0) {
        lua_pushnil(L);
        lua_pushstring(L, media_error(r, why, sizeof why));
        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int l_close(lua_State *L)
{
    struct source *s = (struct source *)luaL_checkudata(L, 1, SOURCE_MT);

    if (s->src != NULL) {
        media_close(s->src);
        s->src = NULL;
    }

    return 0;
}

static int l_packet_free(lua_State *L)
{
    struct packet *p = (struct packet *)luaL_checkudata(L, 1, PACKET_MT);

    if (p->held) {
        media_packet_free(&p->p);
        p->held = 0;
    }

    return 0;
}

static int l_packet_index(lua_State *L)
{
    struct packet *p = (struct packet *)luaL_checkudata(L, 1, PACKET_MT);
    const char *k = luaL_checkstring(L, 2);

    if (strcmp(k, "free") == 0) {
        lua_pushcfunction(L, l_packet_free);
        return 1;
    }

    if (!p->held) {
        return luaL_error(L, "this packet has been freed");
    }

    if (strcmp(k, "stream") == 0) {
        lua_pushinteger(L, p->p.stream);
    } else if (strcmp(k, "pts_us") == 0) {
        if (p->p.pts_us == MEDIA_NO_TIME) { lua_pushnil(L); }
        else { lua_pushinteger(L, p->p.pts_us); }
    } else if (strcmp(k, "dts_us") == 0) {
        if (p->p.dts_us == MEDIA_NO_TIME) { lua_pushnil(L); }
        else { lua_pushinteger(L, p->p.dts_us); }
    } else if (strcmp(k, "duration_us") == 0) {
        lua_pushinteger(L, p->p.duration_us);
    } else if (strcmp(k, "key") == 0) {
        lua_pushboolean(L, p->p.key);
    } else if (strcmp(k, "size") == 0) {
        lua_pushinteger(L, (lua_Integer)p->p.size);
    } else {
        lua_pushnil(L);
    }

    return 1;
}

/* For the decoders' door (M6b), which takes a packet from here. */
const struct media_packet *kosmos_media_packet(lua_State *L, int at)
{
    struct packet *p = (struct packet *)luaL_checkudata(L, at, PACKET_MT);

    if (!p->held) {
        luaL_error(L, "this packet has been freed");
    }

    return &p->p;
}

/*
 * **A decoder from the door** (M6b). `src:decoder(i [, threads])` opens one
 * for stream `i` (0 first, as `media_stream` counts). Then:
 *
 *   dec:send(packet)       a packet in; `dec:send()` with none, at the end,
 *                          to have it give up what it holds. true; false
 *                          when its frames must come out before it takes
 *                          this one, which is then sent again; or nil and
 *                          why
 *   dec:picture(surface)   the next picture written onto `surface` - its
 *                          time in microseconds, `math.mininteger` where
 *                          the film says none - or false when it wants
 *                          another packet, or nil when everything has come
 *                          out, or nil and why
 *   dec:sound()            the next sound: an address and a length of 16-bit
 *                          interleaved PCM in the decoder's own memory,
 *                          valid until the next call, and its time and
 *                          frames - for `player:frame(at, n)`, as
 *                          `gfx.jpeg` takes a frame; false, nil, or nil and
 *                          why, as `picture`
 *   dec:flush()            what it holds let go, after a seek
 *   dec:close()
 *
 * **A picture goes straight onto the surface** through `gfx_draw_i420`,
 * from the decoder's planes: 4:2:0 of 8 bits, and of 10 or 12 bits
 * narrowed into planes of the decoder's own first. Another layout - 4:2:2,
 * 4:4:4, NV12 - is refused with a sentence until something plays one.
 * HD pictures are BT.709 and smaller ones BT.601, studio range: the kit does
 * not say a film's matrix yet, and that is what nearly every film is.
 *
 * **A sound goes into the Playback Kit as PCM**, its WAV kind: the decoder
 * makes float, and this turns it into 16-bit, rounded and clipped, in a
 * buffer of its own that grows to the largest frame it has met.
 */

#define DECODER_MT "kosmos.media.decoder"

struct decoder {
    struct media_decoder *d;
    int kind;
    int16_t *pcm;
    size_t pcm_room;                    /* samples */
    uint8_t *narrow[3];                 /* 10- and 12-bit pictures, made 8 */
    size_t narrow_room[3];
};

static struct decoder *check_decoder(lua_State *L)
{
    struct decoder *d = (struct decoder *)luaL_checkudata(L, 1, DECODER_MT);

    if (d->d == NULL) {
        luaL_error(L, "this decoder is closed");
    }

    return d;
}

static int l_decoder(lua_State *L)
{
    struct source *s = check_source(L);
    lua_Integer i = luaL_checkinteger(L, 2);
    lua_Integer threads = luaL_optinteger(L, 3, 0);
    struct media_stream m;
    struct decoder *d;
    char why[160] = { 0 };

    if (i < 0 || i >= media_stream_count(s->src) || media_stream(s->src, (int)i, &m) != 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "this film has no stream %d", (int)i);
        return 2;
    }

    d = (struct decoder *)lua_newuserdatauv(L, sizeof *d, 1);
    memset(d, 0, sizeof *d);
    luaL_setmetatable(L, DECODER_MT);

    /* The source outlives its decoder: the decoder holds it. */
    lua_pushvalue(L, 1);
    lua_setiuservalue(L, -2, 1);

    d->kind = m.kind;
    d->d = media_decoder_open(s->src, (int)i, (int)threads, why, sizeof why);

    if (d->d == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, why[0] ? why : "this stream will not decode");
        return 2;
    }

    return 1;
}

static int l_send(lua_State *L)
{
    struct decoder *d = check_decoder(L);
    const struct media_packet *p = lua_isnoneornil(L, 2) ? NULL : kosmos_media_packet(L, 2);
    int r = media_decode(d->d, p);
    char why[160];

    if (r == -11) {                     /* EAGAIN: its frames out first */
        lua_pushboolean(L, 0);
        return 1;
    }

    if (r < 0) {
        lua_pushnil(L);
        lua_pushstring(L, media_error(r, why, sizeof why));
        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* After `media_frame`: false for "another packet", nil for "all out", nil
 * and why for a fault. 0 when there is a frame to go on with. */
static int not_a_frame(lua_State *L, int r)
{
    char why[160];

    if (r == 1) {
        return 0;
    }

    if (r == 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    lua_pushnil(L);

    if (r == -1) {
        return 1;
    }

    lua_pushstring(L, media_error(r, why, sizeof why));
    return 2;
}

static uint8_t *room(uint8_t **buf, size_t *have, size_t want)
{
    if (*have < want) {
        uint8_t *grown = realloc(*buf, want);

        if (grown == NULL) {
            return NULL;
        }

        *buf = grown;
        *have = want;
    }

    return *buf;
}

static int l_picture(lua_State *L)
{
    struct decoder *d = check_decoder(L);
    struct surface *out = gfx_surface_check(L, 2);
    struct media_frame f;
    const uint8_t *plane[3];
    int stride[3];
    int r, n, i;

    if (d->kind != MEDIA_VIDEO) {
        return luaL_error(L, "this decoder's stream is not a picture");
    }

    r = media_frame(d->d, &f);
    n = not_a_frame(L, r);

    if (n > 0) {
        return n;
    }

    if (f.format != MEDIA_YUV420 || f.planes != 3) {
        lua_pushnil(L);
        lua_pushstring(L, "this film's pictures are not 4:2:0, which is all this plays yet");
        return 2;
    }

    for (i = 0; i < 3; i++) {
        plane[i] = f.plane[i];
        stride[i] = f.stride[i];
    }

    /* Ten or twelve bits a sample, two bytes each: the top eight kept. */
    if (f.bits > 8) {
        for (i = 0; i < 3; i++) {
            int w = f.plane_bytes[i] / 2, h = f.plane_rows[i], x, y;
            uint8_t *to = room(&d->narrow[i], &d->narrow_room[i], (size_t)w * (size_t)h);

            if (to == NULL) {
                lua_pushnil(L);
                lua_pushstring(L, "no memory to narrow this picture into");
                return 2;
            }

            for (y = 0; y < h; y++) {
                const uint16_t *row = (const uint16_t *)(const void *)(f.plane[i]
                                                                       + (size_t)y * (size_t)f.stride[i]);

                for (x = 0; x < w; x++) {
                    to[(size_t)y * (size_t)w + (size_t)x] = (uint8_t)(row[x] >> (f.bits - 8));
                }
            }

            plane[i] = to;
            stride[i] = w;
        }
    }

    gfx_draw_i420(out, plane, stride, (unsigned)f.width, (unsigned)f.height,
                  f.height >= 720, false);

    lua_pushinteger(L, f.pts_us);
    return 1;
}

static int l_sound(lua_State *L)
{
    struct decoder *d = check_decoder(L);
    struct media_frame f;
    size_t samples, k;
    int r, n;

    if (d->kind != MEDIA_AUDIO) {
        return luaL_error(L, "this decoder's stream is not a sound");
    }

    r = media_frame(d->d, &f);
    n = not_a_frame(L, r);

    if (n > 0) {
        return n;
    }

    samples = (size_t)f.samples * (size_t)f.channels;

    if (d->pcm_room < samples) {
        int16_t *grown = realloc(d->pcm, samples * sizeof *grown);

        if (grown == NULL) {
            lua_pushnil(L);
            lua_pushstring(L, "no memory for this sound");
            return 2;
        }

        d->pcm = grown;
        d->pcm_room = samples;
    }

    for (k = 0; k < samples; k++) {
        float v = f.pcm[k] * 32767.0f;

        v = v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v;
        d->pcm[k] = (int16_t)(v < 0 ? v - 0.5f : v + 0.5f);
    }

    lua_pushinteger(L, (lua_Integer)(uintptr_t)d->pcm);
    lua_pushinteger(L, (lua_Integer)(samples * sizeof *d->pcm));
    lua_pushinteger(L, f.pts_us);
    lua_pushinteger(L, f.samples);
    return 4;
}

static int l_flush(lua_State *L)
{
    media_decoder_flush(check_decoder(L)->d);
    return 0;
}

static int l_decoder_close(lua_State *L)
{
    struct decoder *d = (struct decoder *)luaL_checkudata(L, 1, DECODER_MT);
    int i;

    if (d->d != NULL) {
        media_decoder_close(d->d);
        d->d = NULL;
    }

    free(d->pcm);
    d->pcm = NULL;
    d->pcm_room = 0;

    for (i = 0; i < 3; i++) {
        free(d->narrow[i]);
        d->narrow[i] = NULL;
        d->narrow_room[i] = 0;
    }

    return 0;
}

void kosmos_media_kit(lua_State *L)
{
    static const luaL_Reg methods[] = {
        { "streams", l_streams },
        { "read",    l_read },
        { "seek",    l_seek },
        { "decoder", l_decoder },
        { "close",   l_close },
        { NULL, NULL }
    };
    static const luaL_Reg decoder_methods[] = {
        { "send",    l_send },
        { "picture", l_picture },
        { "sound",   l_sound },
        { "flush",   l_flush },
        { "close",   l_decoder_close },
        { NULL, NULL }
    };
    static const luaL_Reg api[] = {
        { "version", l_version },
        { "open",    l_open },
        { NULL, NULL }
    };

    if (luaL_newmetatable(L, SOURCE_MT)) {
        luaL_newlib(L, methods);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_close);
        lua_setfield(L, -2, "__gc");
    }

    lua_pop(L, 1);

    if (luaL_newmetatable(L, PACKET_MT)) {
        lua_pushcfunction(L, l_packet_index);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_packet_free);
        lua_setfield(L, -2, "__gc");
    }

    lua_pop(L, 1);

    if (luaL_newmetatable(L, DECODER_MT)) {
        luaL_newlib(L, decoder_methods);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_decoder_close);
        lua_setfield(L, -2, "__gc");
    }

    lua_pop(L, 1);
    luaL_newlib(L, api);
}
