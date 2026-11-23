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
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

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

void kosmos_media_kit(lua_State *L)
{
    static const luaL_Reg methods[] = {
        { "streams", l_streams },
        { "read",    l_read },
        { "seek",    l_seek },
        { "close",   l_close },
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
    luaL_newlib(L, api);
}
