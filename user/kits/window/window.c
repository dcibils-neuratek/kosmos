/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Window Kit (`docs/windowkit.md`): `kosmos_window.h`'s five calls.
 *
 * **What happens once is `ui.lua`'s; what happens every frame is C's.**
 * Opening, the two buffers and a resize's new region - `take_size` - are
 * `ui.lua`'s, called on the process's Lua state rather than written again
 * here (W1). `commit` and `poll`, sixty times a second, are `wmproto.h`'s
 * structs, sent from here to the window manager's endpoint with nothing
 * built in Lua (W2): over tables they made 1,216 bytes of garbage a frame
 * in an application that otherwise makes none (`testing.md` 18.433).
 *
 * Every Kosmos process that runs C a project wrote is entered from Lua, so
 * there is a state to call on: `kosmos_lua_state`, the one `lua_glue.c`
 * opens. Errors from Lua are caught here and become a NULL, a 0 or a
 * KW_CLOSE, never a Lua error raised through the application's C.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "kosmos.h"
#include "lua.h"
#include "lauxlib.h"

#include "kosmos_window.h"
#include "wmproto.h"

extern lua_State *kosmos_lua_state;

uint32_t *kosmos_surface_pixels(lua_State *L, int index, unsigned *width,
                                unsigned *height, unsigned *pitch);

void kosmos_window_kit(lua_State *L);

#define QUEUE 32

struct kw_window {
    int win;                    /* registry: ui.lua's window */
    long handle;                /* the window manager's name for it */
    long wm;                    /* its endpoint: the frame path's door */
    int open;
    int band;                   /* the header's band above the surface: its rows */
    unsigned head, count;
    struct kw_event queue[QUEUE];
};

static char why[160] = "";
static int ui_ref = LUA_NOREF;
static unsigned long tick_hz;

static void say(const char *text)
{
    strncpy(why, text != NULL ? text : "?", sizeof why - 1);
    why[sizeof why - 1] = '\0';
}

const char *kw_why(void)
{
    return why;
}

/*
 * The program's own `name` - `use`, `fs` - on the stack, or 0. A program's
 * world is its environment, not Lua's globals (`init.lua`, `env.use`): a
 * library is loaded with the caller's environment and reaches what the
 * caller can and nothing more. So the kit looks where a library would - the
 * `_ENV` of the nearest Lua function that called into this C, which is the
 * program's.
 */
static int program_has(lua_State *L, const char *want)
{
    lua_Debug ar;

    for (int level = 0; lua_getstack(L, level, &ar); level++) {
        lua_getinfo(L, "f", &ar);

        for (int n = 1;; n++) {
            const char *name = lua_getupvalue(L, -1, n);

            if (name == NULL) {
                break;
            }

            if (strcmp(name, "_ENV") == 0 && lua_istable(L, -1)) {
                lua_getfield(L, -1, want);

                if (!lua_isnil(L, -1)) {
                    lua_replace(L, -3);         /* over the function */
                    lua_pop(L, 1);              /* the environment */
                    return 1;
                }

                lua_pop(L, 1);
            }

            lua_pop(L, 1);
        }

        lua_pop(L, 1);
    }

    say("no program's environment above this C to open a window from");
    return 0;
}

/* use(path), kept: the library's table on the stack, or 0 and why. */
static int library(lua_State *L, int *ref, const char *path)
{
    if (*ref == LUA_NOREF) {
        if (!program_has(L, "use")) {
            return 0;
        }

        lua_pushstring(L, path);

        if (lua_pcall(L, 1, 1, 0) != LUA_OK || !lua_istable(L, -1)) {
            say(lua_isstring(L, -1) ? lua_tostring(L, -1) : "a library would not load");
            lua_pop(L, 1);
            return 0;
        }

        *ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, *ref);
    return 1;
}

/* window:<method>(...) with `nargs` already pushed after the method's
 * place: the window and the method are put beneath them. */
static int method(lua_State *L, struct kw_window *w, const char *name, int nargs,
                  int nresults)
{
    int base = lua_gettop(L) - nargs;

    lua_rawgeti(L, LUA_REGISTRYINDEX, w->win);
    lua_getfield(L, -1, name);
    lua_insert(L, base + 1);                    /* the method */
    lua_insert(L, base + 2);                    /* self */

    if (lua_pcall(L, nargs + 1, nresults, 0) != LUA_OK) {
        say(lua_tostring(L, -1));
        lua_pop(L, 1);
        return 0;
    }

    return 1;
}

struct kw_window *kw_open(const char *title, unsigned width, unsigned height,
                          unsigned flags)
{
    lua_State *L = kosmos_lua_state;
    struct kw_window *w;
    int top;

    if (L == NULL) {
        say("this process has no Lua state to open a window from");
        return NULL;
    }

    top = lua_gettop(L);

    if (!library(L, &ui_ref, "/Kosmos/Libraries/ui.lua")) {
        return NULL;
    }

    lua_getfield(L, -1, "window");
    lua_createtable(L, 0, 6);
    lua_pushstring(L, title != NULL ? title : "window");
    lua_setfield(L, -2, "title");
    lua_pushinteger(L, width);
    lua_setfield(L, -2, "w");
    lua_pushinteger(L, height);
    lua_setfield(L, -2, "h");
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "direct");
    lua_pushboolean(L, (flags & KW_RESIZABLE) != 0);
    lua_setfield(L, -2, "resizable");
    lua_pushboolean(L, (flags & KW_CENTRE) != 0);
    lua_setfield(L, -2, "centre");

    if (lua_pcall(L, 1, 2, 0) != LUA_OK) {
        say(lua_tostring(L, -1));
        lua_settop(L, top);
        return NULL;
    }

    if (!lua_istable(L, -2)) {
        say(lua_isstring(L, -1) ? lua_tostring(L, -1) : "the window manager would not open it");
        lua_settop(L, top);
        return NULL;
    }

    lua_pop(L, 1);                              /* the second answer */
    w = calloc(1, sizeof *w);

    if (w == NULL) {
        say("no memory for a window");
        lua_settop(L, top);
        return NULL;
    }

    lua_getfield(L, -1, "handle");
    w->handle = (long)lua_tointeger(L, -1);
    lua_pop(L, 1);

    /*
     * **The band** (one window chrome, step 2): `ui.lua` draws a direct
     * window's header into the top of its surface and hands `surface` the
     * part under it. Its height, once, so the frame path moves a commit
     * down past it and the pointer up past it without asking Lua - a press
     * on the band is the one thing that does.
     */
    lua_getfield(L, -1, "band");

    if (lua_istable(L, -1)) {
        lua_getfield(L, -2, "head_h");
        w->band = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    } else {
        w->band = 0;
    }

    lua_pop(L, 1);
    w->win = luaL_ref(L, LUA_REGISTRYINDEX);
    w->open = 1;
    w->wm = -1;

    /* The window manager's endpoint, once: `fs.capability`, an index this
     * process already holds for `/Running/wm`. */
    if (program_has(L, "fs")) {
        lua_getfield(L, -1, "capability");
        lua_pushstring(L, "/Running/wm");

        if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_isinteger(L, -1)) {
            w->wm = (long)lua_tointeger(L, -1);
        }
    }

    lua_settop(L, top);

    if (w->wm < 0) {
        say("the window manager's endpoint was not found for the frame path");
        kw_close(w);
        return NULL;
    }

    if (tick_hz == 0) {
        struct schedinfo info;

        tick_hz = kosmos_sched_info(&info) == 0 && info.tick_hz > 0 ? info.tick_hz : 250;
    }

    return w;
}

struct kw_surface kw_surface(struct kw_window *w)
{
    lua_State *L = kosmos_lua_state;
    struct kw_surface s = { NULL, 0, 0, 0 };
    int top = lua_gettop(L);

    if (w == NULL || !w->open || !method(L, w, "surface", 0, 1)) {
        lua_settop(L, top);
        return s;
    }

    if (!lua_isnil(L, -1)) {
        s.pixels = kosmos_surface_pixels(L, -1, &s.width, &s.height, &s.pitch);
    }

    /* The window keeps its region; the pixels outlive this userdata. */
    lua_settop(L, top);
    return s;
}

/* One frame request and its answer, `wmproto.h`'s shapes: 0, or the
 * kernel's or the window manager's no. */
static long frame_call(struct kw_window *w, const struct wm_frame_request *rq,
                       struct wm_frame_reply *rp)
{
    static struct message msg, reply;   /* 2 KB each: off the stack */
    long status;

    memset(&msg, 0, offsetof(struct message, data));
    msg.tag = WM_FRAME_TAG;
    msg.length = sizeof *rq;
    memcpy(msg.data, rq, sizeof *rq);

    status = kosmos_call(w->wm, &msg, &reply);

    if (status != 0) {
        return status;
    }

    memset(rp, 0, sizeof *rp);
    memcpy(rp, reply.data, reply.length < sizeof *rp ? reply.length : sizeof *rp);

    if (reply.length < 12 || rp->count > WM_FRAME_EVENTS
        || reply.length < 12 + rp->count * sizeof(struct wm_frame_event)) {
        return WM_FRAME_BAD;
    }

    return rp->error;
}

int kw_commit(struct kw_window *w, unsigned x, unsigned y, unsigned width,
              unsigned height)
{
    lua_State *L = kosmos_lua_state;
    struct wm_frame_request rq;
    struct wm_frame_reply rp;
    int top;

    if (w == NULL || !w->open) {
        return 0;
    }

    /* Under the band, and the band with it from the top when `ui.lua` has
     * just drawn it into this buffer - an integer it leaves in the region,
     * read and cleared here as `draw_into` is written. */
    if (w->band > 0) {
        y += (unsigned)w->band;
        top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, w->win);
        lua_getfield(L, -1, "region");

        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "band_fresh");

            if (lua_tointeger(L, -1) == 1) {
                lua_pop(L, 1);
                lua_pushinteger(L, 0);
                lua_setfield(L, -2, "band_fresh");
                height += y;
                x = 0;
                y = 0;
                lua_getfield(L, -2, "w");
                width = (unsigned)lua_tointeger(L, -1);
            }
        }

        lua_settop(L, top);
    }

    rq = (struct wm_frame_request){ WM_FRAME_COMMIT, (uint32_t)w->handle,
                                    (int32_t)x, (int32_t)y, width, height, 0 };

    if (frame_call(w, &rq, &rp) != 0) {
        return 0;
    }

    /* The buffer to draw into next, where `window:surface` reads it: an
     * integer into a field that is there, which allocates nothing. */
    top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, w->win);
    lua_getfield(L, -1, "region");

    if (lua_istable(L, -1)) {
        lua_pushinteger(L, rp.draw_into);
        lua_setfield(L, -2, "draw_into");
    }

    lua_settop(L, top);
    return 1;
}

static void queue(struct kw_window *w, const struct kw_event *e)
{
    if (w->count < QUEUE) {
        w->queue[(w->head + w->count) % QUEUE] = *e;
        w->count++;
    }
}

/* A resize: the region the new size, made by `ui.lua` - which happens
 * because a person dragged a grip, not because a clock came round. */
static void take_size(lua_State *L, struct kw_window *w, int width, int height)
{
    int top = lua_gettop(L);

    /* `height` is the window's; what the application draws is under the
     * band, and the region the band's and that. */
    lua_rawgeti(L, LUA_REGISTRYINDEX, w->win);
    lua_pushinteger(L, width);
    lua_setfield(L, -2, "w");
    lua_pushinteger(L, height - w->band);
    lua_setfield(L, -2, "h");
    lua_settop(L, top);

    lua_pushinteger(L, width);
    lua_pushinteger(L, height);
    method(L, w, "take_size", 2, 0);
    lua_settop(L, top);
}

int kw_poll(struct kw_window *w, struct kw_event *e, unsigned wait_ms)
{
    if (w == NULL) {
        return 0;
    }

    if (w->count == 0 && w->open) {
        /* Milliseconds here, the scheduler's ticks on the wire: the one
         * conversion, rounded up so a short wait is not no wait. */
        struct wm_frame_request rq = { WM_FRAME_POLL, (uint32_t)w->handle, 0, 0, 0, 0,
                                       (uint32_t)(((unsigned long)wait_ms * tick_hz + 999) / 1000) };
        struct wm_frame_reply rp;

        if (frame_call(w, &rq, &rp) != 0) {
            /* No answer, or no window: the window manager has gone, or the
             * window with it - which is how an application here ends. */
            struct kw_event gone = { .type = KW_CLOSE };

            queue(w, &gone);
        } else {
            for (uint32_t i = 0; i < rp.count; i++) {
                const struct wm_frame_event *f = &rp.events[i];
                struct kw_event ev;

                memset(&ev, 0, sizeof ev);

                switch (f->type) {
                case WM_EV_KEY:
                    ev.type = KW_KEY;
                    ev.key = f->a;
                    break;
                case WM_EV_POINTER:
                    /* On the band: a left press takes hold of the window -
                     * moved, or maximised on a second - and nothing of it
                     * reaches the application; under it, moved up. */
                    if (f->b < w->band) {
                        if (f->action == WM_ACT_PRESS && f->button != WM_BUTTON_RIGHT) {
                            lua_State *L = kosmos_lua_state;
                            int top = lua_gettop(L);

                            lua_pushinteger(L, f->a);
                            lua_pushinteger(L, f->b);
                            method(L, w, "take_hold", 2, 0);
                            lua_settop(L, top);
                        }

                        continue;
                    }

                    ev.type = KW_POINTER;
                    ev.action = f->action == WM_ACT_PRESS ? KW_PRESS
                              : f->action == WM_ACT_RELEASE ? KW_RELEASE : KW_MOVE;
                    ev.button = f->button == WM_BUTTON_RIGHT ? KW_RIGHT : KW_LEFT;
                    ev.x = f->a;
                    ev.y = f->b - w->band;
                    break;
                case WM_EV_WHEEL:
                    ev.type = KW_WHEEL;
                    ev.x = f->a;
                    ev.y = f->b - w->band;
                    ev.amount = f->c;
                    break;
                case WM_EV_RESIZE:
                    ev.type = KW_RESIZE;
                    ev.width = f->a;
                    ev.height = f->b - w->band;
                    take_size(kosmos_lua_state, w, f->a, f->b);
                    break;
                case WM_EV_CLOSE:
                    ev.type = KW_CLOSE;
                    break;
                default:
                    continue;
                }

                queue(w, &ev);
            }
        }
    }

    if (w->count == 0) {
        return 0;
    }

    *e = w->queue[w->head];
    w->head = (w->head + 1) % QUEUE;
    w->count--;
    return 1;
}

void kw_close(struct kw_window *w)
{
    lua_State *L = kosmos_lua_state;
    int top;

    if (w == NULL) {
        return;
    }

    top = lua_gettop(L);

    if (w->open) {
        method(L, w, "close", 0, 0);
        w->open = 0;
    }

    luaL_unref(L, LUA_REGISTRYINDEX, w->win);
    lua_settop(L, top);
    free(w);
}

/* `/Kosmos/Kits/window`: the kit's door is C's (`kosmos_window.h`); from
 * Lua it says what it is, and `ui.window` is Lua's door to the same. */
void kosmos_window_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushstring(L, "kosmos_window.h");
    lua_setfield(L, -2, "header");
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "step");
}
