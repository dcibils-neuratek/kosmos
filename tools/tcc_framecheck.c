/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Window Kit's frame path, held (`run_tcc.py`, `docs/windowkit.md` W2):
 * built inside Kosmos by TinyCC from the developer files, and run on the
 * desktop. A window, and 100 frames of commit and poll with the collector
 * stopped: the Lua heap read before and after, which W2 holds to nothing.
 * `framecheck.lua` asks the window manager the bad questions.
 */

#include <string.h>

#include "kosmos_kit.h"
#include "kosmos_window.h"

static int l_frames(lua_State *L)
{
    struct kw_window *w = kw_open("Frame check", 200, 120, 0);
    struct kw_event e;
    int before, after;

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
    kw_close(w);

    lua_pushinteger(L, after - before);
    return 1;
}

KOSMOS_KIT(framecheck)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_frames);
    lua_setfield(L, -2, "frames");
}
