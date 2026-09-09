/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Lite XL's `system` module, and the three it does without.
 *
 * **This is the second and last upstream file the build replaces**, and
 * unlike `renderer.c` the reason is not performance or a missing library.
 * It is `CLAUDE.md`:
 *
 *     What is forbidden is a POSIX personality: fork, exec, signal, pipe,
 *     socket, select, ioctl, unistd.h, global file descriptors, or any
 *     path tree reachable without a namespace.
 *
 * `api/system.c` includes `<unistd.h>`, `<dirent.h>` and `<sys/stat.h>`,
 * and calls `opendir`, `stat`, `chdir`, `realpath`, `mkdir` and `remove`.
 * Shimming those would be building precisely the thing that rule forbids -
 * and it could not be done honestly in any case: `stat("/foo")` has no
 * meaning here, because there is no global tree for the path to be in.
 *
 *--------------------------------------------------------------------------
 * What the module turned out to be
 *
 * Almost none of it is computation. Reading a directory, taking the
 * clipboard, moving a window, waiting for a key - **every one of those is a
 * conversation with a server**, and on this system a conversation is had
 * from Lua, where the namespace and the window manager's endpoint are.
 *
 * So this file is a *shape* rather than an implementation. It registers the
 * thirty-two names Lite XL expects, implements the handful that really are
 * computation, and forwards the rest to a host table the Kosmos side
 * installs with `litexl_set_host`. C keeps the interface; Lua does the
 * talking. That is the same division `doom_kosmos.c` arrived at, one layer
 * up.
 *
 * The alternative - C reaching the servers directly - would mean the
 * capability plumbing of a whole second userland written in C to avoid
 * writing Lua, which is the language the rest of this system's policy is
 * already in.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "litexl/SDL.h"

/*--------------------------------------------------------------------------
 * The host: Kosmos's half, in Lua
 *------------------------------------------------------------------------*/

/*
 * Kept in the registry rather than in a C global, so that it lives and dies
 * with the `lua_State` it belongs to and cannot outlive it.
 */
static const char *const HOST_KEY = "kosmos.litexl.host";

void litexl_set_host(lua_State *L, int index)
{
    lua_pushvalue(L, index);
    lua_setfield(L, LUA_REGISTRYINDEX, HOST_KEY);
}

/*
 * Call `host.<name>(...)` with the `n` values on top of the stack, and
 * leave `results` behind.
 *
 * **A missing host function is a Lua error, not a silent nil.** Every name
 * forwarded here is one Lite XL will actually call, so an absent one is a
 * port that is not finished - and finding that out at the call site, with
 * the name in the message, is worth more than a `nil` travelling somewhere
 * else to fail.
 */
static int host_call(lua_State *L, const char *name, int n, int results)
{
    lua_getfield(L, LUA_REGISTRYINDEX, HOST_KEY);

    if (!lua_istable(L, -1)) {
        return luaL_error(L, "litexl: no host installed for system.%s", name);
    }

    lua_getfield(L, -1, name);

    if (!lua_isfunction(L, -1)) {
        return luaL_error(L, "litexl: the host has no system.%s", name);
    }

    /*
     * The arguments are already at 1..n; the host table and the function
     * are on top. Drop the table, then slide the function underneath them,
     * which is the stack `lua_call` wants.
     */
    lua_remove(L, -2);
    lua_insert(L, 1);

    lua_call(L, n, results);

    return results;
}

/* The common shapes, so each binding below is one line of intent. */
static int forward(lua_State *L, const char *name, int results)
{
    return host_call(L, name, lua_gettop(L), results);
}

/*--------------------------------------------------------------------------
 * The ones that really are computation
 *------------------------------------------------------------------------*/

/* The Lua binding, which is the whole of what this file adds to it. */
static int f_fuzzy_match(lua_State *L)
{
    const char *hay    = luaL_checkstring(L, 1);
    const char *needle = luaL_checkstring(L, 2);
    int         score;

    if (!litexl_fuzzy_match(hay, needle, lua_toboolean(L, 3), &score)) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushinteger(L, score);
    return 1;
}

static int f_path_compare(lua_State *L)
{
    lua_pushboolean(L, litexl_path_before(
        luaL_checkstring(L, 1), strcmp(luaL_checkstring(L, 2), "dir") == 0,
        luaL_checkstring(L, 3), strcmp(luaL_checkstring(L, 4), "dir") == 0));

    return 1;
}

/*--------------------------------------------------------------------------
 * The ones that are honestly refused
 *------------------------------------------------------------------------*/

/*
 * `system.exec` and `system.load_native_plugin`.
 *
 * **They fail rather than pretending**, and the message says why so that a
 * plugin author reads a reason instead of a mystery. A subprocess would
 * want `fork`/`exec` with pipes, which `CLAUDE.md` forbids outright; a
 * native plugin would want `dlopen`, and there is no dynamic linking here
 * at all - `design.md` 10 records that hot reload was removed for the same
 * reason.
 */
static int f_exec(lua_State *L)
{
    (void)L;
    return luaL_error(L, "Kosmos has no subprocesses: system.exec is not "
                         "available, and plugins that shell out will not work");
}

static int f_load_native_plugin(lua_State *L)
{
    (void)L;
    return luaL_error(L, "Kosmos has no dynamic linking: native plugins "
                         "cannot be loaded");
}

/*--------------------------------------------------------------------------
 * Everything else, which is a question for a server
 *------------------------------------------------------------------------*/

#define FORWARD(fn, name, results)                       \
    static int fn(lua_State *L)                          \
    {                                                    \
        return forward(L, name, results);                \
    }

FORWARD(f_poll_event,          "poll_event",          1)
FORWARD(f_wait_event,          "wait_event",          1)
FORWARD(f_set_cursor,          "set_cursor",          0)
FORWARD(f_set_window_title,    "set_window_title",    0)
FORWARD(f_set_window_mode,     "set_window_mode",     0)
FORWARD(f_get_window_mode,     "get_window_mode",     1)
FORWARD(f_set_window_bordered, "set_window_bordered", 0)
FORWARD(f_set_window_hit_test, "set_window_hit_test", 0)
FORWARD(f_get_window_size,     "get_window_size",     4)
FORWARD(f_set_window_size,     "set_window_size",     0)
FORWARD(f_set_text_input_rect, "set_text_input_rect", 0)
FORWARD(f_clear_ime,           "clear_ime",           0)
FORWARD(f_window_has_focus,    "window_has_focus",    1)
FORWARD(f_raise_window,        "raise_window",        0)
FORWARD(f_show_fatal_error,    "show_fatal_error",    0)
FORWARD(f_rmdir,               "rmdir",               2)
FORWARD(f_chdir,               "chdir",               0)
FORWARD(f_mkdir,               "mkdir",               2)
FORWARD(f_list_dir,            "list_dir",            2)
FORWARD(f_absolute_path,       "absolute_path",       1)
FORWARD(f_get_file_info,       "get_file_info",       2)
FORWARD(f_get_clipboard,       "get_clipboard",       1)
FORWARD(f_set_clipboard,       "set_clipboard",       0)
FORWARD(f_get_process_id,      "get_process_id",      1)
FORWARD(f_get_time,            "get_time",            1)
FORWARD(f_sleep,               "sleep",               0)
FORWARD(f_set_window_opacity,  "set_window_opacity",  1)
FORWARD(f_get_fs_type,         "get_fs_type",         1)

static const luaL_Reg system_lib[] = {
    { "poll_event",          f_poll_event          },
    { "wait_event",          f_wait_event          },
    { "set_cursor",          f_set_cursor          },
    { "set_window_title",    f_set_window_title    },
    { "set_window_mode",     f_set_window_mode     },
    { "get_window_mode",     f_get_window_mode     },
    { "set_window_bordered", f_set_window_bordered },
    { "set_window_hit_test", f_set_window_hit_test },
    { "get_window_size",     f_get_window_size     },
    { "set_window_size",     f_set_window_size     },
    { "set_text_input_rect", f_set_text_input_rect },
    { "clear_ime",           f_clear_ime           },
    { "window_has_focus",    f_window_has_focus    },
    { "raise_window",        f_raise_window        },
    { "show_fatal_error",    f_show_fatal_error    },
    { "rmdir",               f_rmdir               },
    { "chdir",               f_chdir               },
    { "mkdir",               f_mkdir               },
    { "list_dir",            f_list_dir            },
    { "absolute_path",       f_absolute_path       },
    { "get_file_info",       f_get_file_info       },
    { "get_clipboard",       f_get_clipboard       },
    { "set_clipboard",       f_set_clipboard       },
    { "get_process_id",      f_get_process_id      },
    { "get_time",            f_get_time            },
    { "sleep",               f_sleep               },
    { "exec",                f_exec                },
    { "fuzzy_match",         f_fuzzy_match         },
    { "set_window_opacity",  f_set_window_opacity  },
    { "load_native_plugin",  f_load_native_plugin  },
    { "path_compare",        f_path_compare        },
    { "get_fs_type",         f_get_fs_type         },
    { NULL, NULL }
};

int luaopen_system(lua_State *L)
{
    luaL_newlib(L, system_lib);
    return 1;
}

/*--------------------------------------------------------------------------
 * The three modules this port does without
 *
 * Registered rather than absent, because `api.c` names them and Lite XL's
 * Lua does `require "process"` at the top of files it loads whether or not
 * it goes on to use one. An empty table lets the require succeed and the
 * *use* fail, with a message, at the point somebody actually asks for
 * something impossible.
 *------------------------------------------------------------------------*/

static int f_unavailable(lua_State *L)
{
    const char *what = lua_tostring(L, lua_upvalueindex(1));

    return luaL_error(L, "%s is not available in this build of Lite XL "
                         "on Kosmos", what);
}

static void refuse(lua_State *L, const char *table, const char *const *names)
{
    int i;

    lua_newtable(L);

    for (i = 0; names[i] != NULL; i++) {
        lua_pushfstring(L, "%s.%s", table, names[i]);
        lua_pushcclosure(L, f_unavailable, 1);
        lua_setfield(L, -2, names[i]);
    }
}

int luaopen_process(lua_State *L)
{
    /* Subprocesses. `CLAUDE.md` forbids the personality they would need. */
    static const char *const names[] = { "start", "strerror", NULL };

    refuse(L, "process", names);
    return 1;
}

/*
 * `dirmonitor`, which **cannot refuse**, and finding that out is what the
 * Lua load test is for.
 *
 * The first version of this returned a table whose functions raised, on the
 * grounds that Kosmos cannot watch a directory. `core.init()` then died in
 * `core/dirwatch.lua:41`: the editor makes a monitor at startup and indexes
 * it, whether or not anything is ever watched. A module that refuses is
 * only honest if nobody needs it to exist.
 *
 * So it answers the way upstream's own `dummy.c` does - the nine-line file
 * it ships for platforms with no inotify, kqueue or FSEvents. "single"
 * mode, -1 from `watch`, nothing from `check`; the editor then rescans when
 * it wants to know rather than being told.
 */
static int f_dirmonitor_mode(lua_State *L)
{
    lua_pushstring(L, "single");
    return 1;
}

static int f_dirmonitor_watch(lua_State *L)
{
    lua_pushinteger(L, -1);
    return 1;
}

static int f_dirmonitor_check(lua_State *L)
{
    lua_pushboolean(L, 0);      /* nothing changed, as far as anyone knows */
    return 1;
}

static int f_dirmonitor_new(lua_State *L)
{
    static const luaL_Reg monitor[] = {
        { "mode",    f_dirmonitor_mode  },
        { "watch",   f_dirmonitor_watch },
        { "unwatch", f_dirmonitor_watch },
        { "check",   f_dirmonitor_check },
        { NULL, NULL }
    };

    luaL_newlib(L, monitor);
    return 1;
}

int luaopen_dirmonitor(lua_State *L)
{
    static const luaL_Reg lib[] = {
        { "new", f_dirmonitor_new },
        { NULL, NULL }
    };

    luaL_newlib(L, lib);
    return 1;
}

int luaopen_regex(lua_State *L)
{
    /*
     * PCRE2, which is a second large dependency and is not here yet.
     *
     * `tokenizer.lua` takes either a Lua pattern *or* a regex, and four of
     * the nine bundled languages use the regex form - so what this costs is
     * syntax highlighting on those four. `docs/litexl.md` says so, and the
     * error says it at the point a syntax file asks.
     */
    static const char *const names[] = { "compile", "find", "gsub",
                                         "find_offsets", NULL };

    refuse(L, "regex", names);
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "ANCHORED");
    return 1;
}

/*--------------------------------------------------------------------------
 * The kit, which is how Lite XL reaches a Kosmos program at all
 *
 * **`main.c` is not compiled, and `api_load_libs` was its job.** Nothing
 * else calls it, so in a Kosmos process the modules Lite XL's Lua expects -
 * `system`, `renderer`, `utf8extra` and the rest - simply would not exist.
 *
 * A kit is the door this system already has for that: `use("/kits/litexl")`
 * asks `sys.kit`, which calls the function below, and it registers the same
 * six modules `main.c` would have. They arrive as globals, which is what
 * `luaL_requiref(L, name, fn, 1)` does and what `start.lua` relies on.
 *
 * Reached through the namespace rather than as a global, like every other
 * kit, so the rule holds: a program that was not handed this cannot get an
 * editor's worth of C by naming it.
 *------------------------------------------------------------------------*/

void api_load_libs(lua_State *L);

/*
 * A font's bytes have to outlive the call.
 *
 * `stb_truetype` reads the file for as long as the face exists and does not
 * copy it, so the Lua string handed in here must not be collected. It is
 * anchored in a registry table; nothing removes it, which is correct for
 * the two or three faces an editor opens and would not be for a font
 * manager.
 */
static const char *const FONTS_KEY = "kosmos.litexl.fonts";

static int l_provide_font(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    size_t      len;
    const char *bytes = luaL_checklstring(L, 2, &len);

    lua_getfield(L, LUA_REGISTRYINDEX, FONTS_KEY);

    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, FONTS_KEY);
    }

    lua_pushvalue(L, 2);
    lua_setfield(L, -2, path);      /* anchored for as long as the state */
    lua_pop(L, 1);

    litexl_font_provide(path, bytes, len);
    return 0;
}

/* `attach_window(pixels, w, h, pitch)` - a `gfx` surface's own memory. */
uint32_t *kosmos_surface_pixels(lua_State *L, int index,
                                unsigned *w, unsigned *h, unsigned *pitch);

static int l_attach_window(lua_State *L)
{
    unsigned  w, h, pitch;
    uint32_t *pixels = kosmos_surface_pixels(L, 1, &w, &h, &pitch);

    litexl_window_attach(pixels, (int)w, (int)h, (int)pitch);
    return 0;
}

/*
 * `take_damage()` - what changed since the last frame, as a flat list of
 * x, y, w, h, or `true` when it is the whole window.
 *
 * Flat rather than a table per rectangle: this is called once a frame and
 * four numbers in a table each is four allocations per rectangle for the
 * collector to walk. `CLAUDE.md`'s note about `wait_input` allocating 3.6 KB
 * a pass is the same lesson.
 */
static int l_take_damage(lua_State *L)
{
    SDL_Rect rects[64];
    bool     whole = false;
    int      n     = litexl_damage_take(rects, 64, &whole);
    int      i;

    if (whole) {
        lua_pushboolean(L, 1);
        return 1;
    }

    lua_createtable(L, n * 4, 0);

    for (i = 0; i < n; i++) {
        lua_pushinteger(L, rects[i].x); lua_rawseti(L, -2, i * 4 + 1);
        lua_pushinteger(L, rects[i].y); lua_rawseti(L, -2, i * 4 + 2);
        lua_pushinteger(L, rects[i].w); lua_rawseti(L, -2, i * 4 + 3);
        lua_pushinteger(L, rects[i].h); lua_rawseti(L, -2, i * 4 + 4);
    }

    return 1;
}

static int l_set_host(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    litexl_set_host(L, 1);
    return 0;
}

void kosmos_litexl_kit(lua_State *L)
{
    static const luaL_Reg lib[] = {
        { "set_host",      l_set_host      },
        { "attach_window", l_attach_window },
        { "provide_font",  l_provide_font  },
        { "take_damage",   l_take_damage   },
        { NULL, NULL }
    };

    /* The six modules `main.c` would have registered, as globals. */
    api_load_libs(L);

    luaL_newlib(L, lib);
}
