/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Window Kit's frame path, held (`run_tcc.py`, `docs/windowkit.md` W2):
 * built inside Kosmos by TinyCC from the developer files, and run on the
 * desktop:
 *
 *   - **nothing allocated in Lua a frame**: 100 frames of commit and poll
 *     with the collector stopped, the heap read before and after (W2);
 *   - **the window manager receives what it expects**: an operation the
 *     shape has not got, a request too short, and a window that is not
 *     there, sent from here with `kosmos_call` and `wmproto.h` - which is
 *     also the proof that a C app built inside Kosmos can include
 *     `kosmos.h` and make a system call (`syscall-aarch64.h`, 18.434).
 */

#include <string.h>

#include "kosmos.h"
#include "kosmos_kit.h"
#include "kosmos_window.h"
#include "wmproto.h"

static struct message msg, reply;

/* A frame request of `length` bytes: the window manager's answer, or -1. */
static int ask(long cap, const struct wm_frame_request *rq, unsigned length)
{
    memset(&msg, 0, sizeof msg);
    msg.tag = WM_FRAME_TAG;
    msg.length = length;
    memcpy(msg.data, rq, length);

    if (kosmos_call(cap, &msg, &reply) != 0 || reply.length < 12) {
        return -1;
    }

    return ((const struct wm_frame_reply *)(const void *)reply.data)->error;
}

static int l_frames(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);   /* fs.capability("/Running/wm") */
    struct kw_window *w = kw_open("Frame check", 200, 120, 0);
    struct kw_event e;
    struct wm_frame_request rq;
    int before, after, bad_op, short_one, no_window;

    if (w == NULL) {
        lua_pushfstring(L, "no window: %s", kw_why());
        return 1;
    }

    lua_gc(L, LUA_GCCOLLECT);
    lua_gc(L, LUA_GCSTOP);
    before = lua_gc(L, LUA_GCCOUNT) * 1024 + lua_gc(L, LUA_GCCOUNTB);

    for (int i = 0; i < 100; i++) {
        struct kw_surface s = kw_surface(w);

        if (s.pixels != NULL) {
            memset(s.pixels, i, (size_t)s.pitch * s.height);
            kw_commit(w, 0, 0, s.width, s.height);
        }

        while (kw_poll(w, &e, 0)) {
        }
    }

    after = lua_gc(L, LUA_GCCOUNT) * 1024 + lua_gc(L, LUA_GCCOUNTB);
    lua_gc(L, LUA_GCRESTART);
    memset(&rq, 0, sizeof rq);
    rq.op = 99;
    rq.window = 1;
    bad_op = ask(cap, &rq, sizeof rq);
    rq.op = WM_FRAME_POLL;
    short_one = ask(cap, &rq, 8);
    rq.window = 0x7fffffff;
    no_window = ask(cap, &rq, sizeof rq);

    kw_close(w);

    lua_pushfstring(L, "FRAMECHECK %d bytes over 100 frames; op 99 %d; short %d; "
                       "no window %d", after - before, bad_op, short_one, no_window);
    return 1;
}

KOSMOS_KIT(framecheck)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_frames);
    lua_setfield(L, -2, "frames");
}
