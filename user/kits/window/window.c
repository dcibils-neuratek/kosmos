/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Window Kit, step W1 (`docs/windowkit.md`): `kosmos_window.h`'s five
 * calls, made over the door Lua apps already use.
 *
 * **Not a second copy of it.** Opening, the two buffers, committing,
 * resizing - `take_size` - and a direct window's own events are `ui.lua`'s,
 * and this kit calls them on the process's Lua state rather than writing
 * them again in C. What is C here is the shape the application sees: a
 * struct for the surface, a struct for each event, a wait in milliseconds.
 * W2 moves the frame path - open, commit, poll - onto a declared shape
 * (`wmproto.h`), and the calls above it do not change.
 *
 * Every Kosmos process that runs C a project wrote is entered from Lua, so
 * there is a state to call on: `kosmos_lua_state`, the one `lua_glue.c`
 * opens. Errors from Lua are caught here and become a NULL, a 0 or a
 * KW_CLOSE, never a Lua error raised through the application's C.
 */

#include <stdlib.h>
#include <string.h>

#include "kosmos.h"
#include "lua.h"
#include "lauxlib.h"

#include "kosmos_window.h"

extern lua_State *kosmos_lua_state;

uint32_t *kosmos_surface_pixels(lua_State *L, int index, unsigned *width,
                                unsigned *height, unsigned *pitch);

void kosmos_window_kit(lua_State *L);

#define QUEUE 32

struct kw_window {
    int win;                    /* registry: ui.lua's window */
    long handle;                /* the window manager's name for it */
    int open;
    unsigned head, count;
    struct kw_event queue[QUEUE];
};

static char why[160] = "";
static int ui_ref = LUA_NOREF, wmproto_ref = LUA_NOREF;
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
 * The program's own `use`, on the stack, or 0. A program's world is its
 * environment, not Lua's globals (`init.lua`, `env.use`): a library is
 * loaded with the caller's environment and reaches what the caller can and
 * nothing more. So the kit looks where a library would - the `_ENV` of the
 * nearest Lua function that called into this C, which is the program's.
 */
static int program_use(lua_State *L)
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
                lua_getfield(L, -1, "use");

                if (lua_isfunction(L, -1)) {
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

    say("no program's `use` above this C to open libraries with");
    return 0;
}

/* use(path), kept: the library's table on the stack, or 0 and why. */
static int library(lua_State *L, int *ref, const char *path)
{
    if (*ref == LUA_NOREF) {
        if (!program_use(L)) {
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
    w->win = luaL_ref(L, LUA_REGISTRYINDEX);
    w->open = 1;
    lua_settop(L, top);

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

int kw_commit(struct kw_window *w, unsigned x, unsigned y, unsigned width,
              unsigned height)
{
    lua_State *L = kosmos_lua_state;
    int top = lua_gettop(L), ok;

    if (w == NULL || !w->open) {
        return 0;
    }

    lua_createtable(L, 0, 4);
    lua_pushinteger(L, x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, y);
    lua_setfield(L, -2, "y");
    lua_pushinteger(L, width);
    lua_setfield(L, -2, "w");
    lua_pushinteger(L, height);
    lua_setfield(L, -2, "h");

    ok = method(L, w, "commit", 1, 1) && lua_toboolean(L, -1);
    lua_settop(L, top);
    return ok;
}

static void queue(struct kw_window *w, const struct kw_event *e)
{
    if (w->count < QUEUE) {
        w->queue[(w->head + w->count) % QUEUE] = *e;
        w->count++;
    }
}

static int field(lua_State *L, int t, const char *name)
{
    int v;

    lua_getfield(L, t, name);
    v = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return v;
}

/* One of the window manager's events, at the top of the stack, as a struct
 * - after the window's own handling, which takes a resize's new surface
 * and anything that was a menu's. */
static void take(lua_State *L, struct kw_window *w)
{
    int ev = lua_gettop(L);
    struct kw_event e;
    const char *type, *s;
    int handled;

    lua_pushvalue(L, ev);
    handled = method(L, w, "direct_event", 1, 1) && lua_toboolean(L, -1);
    lua_settop(L, ev);

    if (handled) {
        return;
    }

    memset(&e, 0, sizeof e);
    lua_getfield(L, ev, "type");
    type = lua_tostring(L, -1);
    lua_pop(L, 1);

    if (type == NULL) {
        return;
    }

    if (strcmp(type, "key") == 0) {
        e.type = KW_KEY;
        e.key = field(L, ev, "code");
    } else if (strcmp(type, "mouse") == 0) {
        e.type = KW_POINTER;
        lua_getfield(L, ev, "action");
        s = lua_tostring(L, -1);
        e.action = s == NULL ? 0 : strcmp(s, "press") == 0 ? KW_PRESS
                 : strcmp(s, "release") == 0 ? KW_RELEASE : KW_MOVE;
        lua_pop(L, 1);
        lua_getfield(L, ev, "button");
        s = lua_tostring(L, -1);
        e.button = s != NULL && strcmp(s, "right") == 0 ? KW_RIGHT : KW_LEFT;
        lua_pop(L, 1);
        e.x = field(L, ev, "x");
        e.y = field(L, ev, "y");
    } else if (strcmp(type, "wheel") == 0) {
        e.type = KW_WHEEL;
        e.amount = field(L, ev, "n");
        e.x = field(L, ev, "x");
        e.y = field(L, ev, "y");
    } else if (strcmp(type, "resize") == 0) {
        e.type = KW_RESIZE;
        e.width = field(L, ev, "w");
        e.height = field(L, ev, "h");
    } else if (strcmp(type, "close") == 0) {
        e.type = KW_CLOSE;
    } else {
        return;
    }

    queue(w, &e);
}

int kw_poll(struct kw_window *w, struct kw_event *e, unsigned wait_ms)
{
    lua_State *L = kosmos_lua_state;
    int top;

    if (w == NULL) {
        return 0;
    }

    if (w->count == 0 && w->open) {
        /* Milliseconds here, the scheduler's ticks on the wire: the one
         * conversion, rounded up so a short wait is not no wait. */
        unsigned long ticks = ((unsigned long)wait_ms * tick_hz + 999) / 1000;

        top = lua_gettop(L);

        if (library(L, &wmproto_ref, "/Kosmos/Libraries/wmproto.lua")) {
            lua_getfield(L, -1, "poll");
            lua_pushinteger(L, w->handle);
            lua_pushinteger(L, (lua_Integer)ticks);

            if (lua_pcall(L, 2, 1, 0) != LUA_OK || !lua_istable(L, -1)) {
                /* No answer: the window manager has gone, and so has the
                 * window - which is how an application here ends. */
                struct kw_event gone = { .type = KW_CLOSE };

                queue(w, &gone);
            } else {
                lua_getfield(L, -1, "events");

                if (lua_istable(L, -1)) {
                    lua_Integer n = (lua_Integer)lua_rawlen(L, -1);

                    for (lua_Integer i = 1; i <= n; i++) {
                        lua_rawgeti(L, -1, i);

                        if (lua_istable(L, -1)) {
                            take(L, w);
                        }

                        lua_pop(L, 1);
                    }
                }
            }
        }

        lua_settop(L, top);
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
