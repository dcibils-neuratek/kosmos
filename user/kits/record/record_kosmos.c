/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Record Kit**: a camera's frames, taken from its ring, into an MP4 of
 * H.264 (`roadmap.md` 6d 8f) - `record_core.c` behind a Lua face.
 *
 *   local record = use("/kits/record")
 *   local bytes = record.work_bytes(640, 480)    -- nil: not recordable
 *   local r, why = record.open{ work = at, work_bytes = n,
 *                               out = at, out_bytes = n,
 *                               width = 640, height = 480, fps = 30 }
 *   r:camera(ring, last)      -- the newest frame, recorded: its number,
 *                             -- or false and "same", "waiting", "stopped",
 *                             -- "mjpeg", or why it could not be
 *   r:bytes(), r:frames()     -- how far it has got
 *   r:close()                 -- the file's length in `out`, or nil and why
 *
 * **Both memories are the caller's regions**, made with `sys.memory` and
 * mapped - the encoder's frames alone outgrow a process's 2 MB heap - and
 * the file, whole in `out`, is written to the disk with one `write_from`
 * when the recording stops. `/lib/camera.lua` does that part.
 *
 * **Timed by the counter at the moment each frame is taken**, so a machine
 * that encodes slower than the camera sends makes a recording with fewer
 * frames that plays at the speed things happened. A recording is never
 * mirrored: this takes the camera's bytes, and the mirror is the window's.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include <kosmos.h>
#include <cameraproto.h>

#include "record_core.h"

#define RECORD_MT "kosmos.record"

struct rec {
    struct recorder *r;
    uint64_t hz;                        /* the counter's, for microseconds */
    bool     closed;
};

static struct rec *check(lua_State *L)
{
    return (struct rec *)luaL_checkudata(L, 1, RECORD_MT);
}

static lua_Integer field(lua_State *L, const char *name)
{
    lua_Integer v;

    lua_getfield(L, 1, name);
    v = luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    return v;
}

static int l_work_bytes(lua_State *L)
{
    size_t n = recorder_work_bytes((unsigned)luaL_checkinteger(L, 1),
                                   (unsigned)luaL_checkinteger(L, 2));

    if (n == 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "a recording is an even width and height, "
                           "4096 at most");
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)n);
    return 1;
}

static int l_open(lua_State *L)
{
    struct sysinfo info;
    struct rec *rec;
    const char *why = "the recording could not start";
    lua_Integer work, work_bytes, out, out_bytes, w, h, fps;

    luaL_checktype(L, 1, LUA_TTABLE);
    work = field(L, "work");
    work_bytes = field(L, "work_bytes");
    out = field(L, "out");
    out_bytes = field(L, "out_bytes");
    w = field(L, "width");
    h = field(L, "height");
    fps = field(L, "fps");

    if (work == 0 || out == 0 || work_bytes <= 0 || out_bytes <= 0
        || w <= 0 || h <= 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "a recording wants its two regions and a size");
        return 2;
    }

    rec = (struct rec *)lua_newuserdatauv(L, sizeof(*rec), 0);
    memset(rec, 0, sizeof(*rec));
    luaL_setmetatable(L, RECORD_MT);

    rec->hz = (kosmos_sysinfo(&info) == 0 && info.counter_hz != 0)
              ? info.counter_hz : 1000000u;
    rec->r = recorder_open((void *)(uintptr_t)work, (size_t)work_bytes,
                           (uint8_t *)(uintptr_t)out, (size_t)out_bytes,
                           (unsigned)w, (unsigned)h,
                           fps > 0 ? (unsigned)fps : 30u, &why);

    if (rec->r == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    }

    return 1;
}

static int l_camera(lua_State *L)
{
    struct rec *rec = check(L);
    lua_Integer at = luaL_checkinteger(L, 2);
    uint32_t last = (uint32_t)luaL_optinteger(L, 3, 0);
    volatile struct camera_ring *ring =
        (volatile struct camera_ring *)(uintptr_t)at;
    uint32_t slot = 0, sequence = 0;
    const char *why = NULL;
    uint64_t now, us;
    bool ok;

    if (rec->r == NULL || rec->closed) {
        lua_pushboolean(L, 0);
        lua_pushliteral(L, "the recording has stopped");
        return 2;
    }

    if (at == 0 || ring->magic != CAMERA_RING_MAGIC) {
        return luaL_error(L, "that is not a camera's region");
    }

    switch (camera_take(ring, last, &slot, &sequence)) {
    case CAMERA_TAKEN:
        break;
    case CAMERA_SAME:
        lua_pushboolean(L, 0);
        lua_pushliteral(L, "same");
        return 2;
    case CAMERA_STOPPED:
        lua_pushboolean(L, 0);
        lua_pushliteral(L, "stopped");
        return 2;
    default:
        lua_pushboolean(L, 0);
        lua_pushliteral(L, "waiting");
        return 2;
    }

    if (ring->pixels != CAMERA_PIXELS_YUY2
        || ring->length[slot] != ring->width * ring->height * 2u) {
        camera_done(ring, sequence);
        lua_pushboolean(L, 0);
        lua_pushliteral(L, "mjpeg");
        return 2;
    }

    now = kosmos_ticks();
    us = (now / rec->hz) * 1000000u + (now % rec->hz) * 1000000u / rec->hz;
    ok = recorder_yuy2(rec->r, camera_slot(ring, slot), us, &why);
    camera_done(ring, sequence);

    if (!ok) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, why);
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)sequence);
    return 1;
}

static int l_bytes(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)recorder_bytes(check(L)->r));
    return 1;
}

static int l_frames(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)recorder_frames(check(L)->r));
    return 1;
}

static int l_close(lua_State *L)
{
    struct rec *rec = check(L);
    const char *why = "the recording has already stopped";
    size_t n = 0;

    if (rec->r != NULL && !rec->closed) {
        rec->closed = true;
        n = recorder_close(rec->r, &why);
    }

    if (n == 0) {
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)n);
    return 1;
}

void kosmos_record_kit(lua_State *L)
{
    static const luaL_Reg methods[] = {
        { "camera", l_camera },
        { "bytes",  l_bytes },
        { "frames", l_frames },
        { "close",  l_close },
        { NULL, NULL }
    };

    static const luaL_Reg api[] = {
        { "work_bytes", l_work_bytes },
        { "open",       l_open },
        { NULL, NULL }
    };

    if (luaL_newmetatable(L, RECORD_MT)) {
        luaL_newlib(L, methods);
        lua_setfield(L, -2, "__index");
    }

    lua_pop(L, 1);
    luaL_newlib(L, api);
}
