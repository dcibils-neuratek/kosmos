/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The door to `jetski.cpp` (`docs/w7.md` W7c): a kit in no image but
 * `jetski.elf`, reached as `use("jetski.elf")` by `jetski.lua`.
 *
 *   open(w, h)                   the page's view; true, or nil and why
 *   resize(w, h)
 *   load(address), back(), forward(), reload(), stop()
 *   pump(ms, into)               WebKit for at most `ms`, or until there
 *                                is a picture; what happened, into `into`
 *   paint(surface, x, y, w, h)   the page, w by h, at x, y in the surface
 *
 * `pump` fills a table the caller keeps rather than making one: it runs on
 * every pass of the window's loop, and a table a pass is garbage the
 * collector has to come for in the middle of a frame.
 */

#include "lua.h"
#include "lauxlib.h"

#include "jetski.h"
#include "kits/gfx/gfx_draw.h"      /* kosmos_surface_pixels */
#include "kits/kitenv.h"            /* kit_capability */

/* `posix_program.h`: where this process's standard error goes, and so
 * the web and network processes' it starts. */
void posix_console(long capability);
long posix_console_capability(void);

static unsigned side(lua_State *L, int index)
{
    lua_Integer v = luaL_checkinteger(L, index);

    luaL_argcheck(L, v > 0 && v <= 16384, index, "1 to 16384");
    return (unsigned)v;
}

static int l_open(lua_State *L)
{
    unsigned w = side(L, 1), h = side(L, 2);

    if (posix_console_capability() < 0) {
        posix_console(kit_capability(L, "/Devices/console"));
    }
    if (jetski_open(w, h) != 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "the page's view was not made");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_resize(lua_State *L)
{
    jetski_resize(side(L, 1), side(L, 2));
    return 0;
}

/* web.frame(x, y, w, h): where the window is, and its whole size. */
static int l_frame(lua_State *L)
{
    jetski_frame((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2),
                 side(L, 3), side(L, 4));
    return 0;
}

static int l_load(lua_State *L)
{
    jetski_load(luaL_checkstring(L, 1));
    return 0;
}

static int l_back(lua_State *L)    { (void)L; jetski_back(); return 0; }
static int l_forward(lua_State *L) { (void)L; jetski_forward(); return 0; }
static int l_reload(lua_State *L)  { (void)L; jetski_reload(); return 0; }
static int l_stop(lua_State *L)    { (void)L; jetski_stop(); return 0; }

static void flag(lua_State *L, const char *name, int on)
{
    lua_pushboolean(L, on);
    lua_setfield(L, 2, name);
}

static int l_pump(lua_State *L)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    struct jetski_state st;

    luaL_argcheck(L, ms >= 0 && ms <= 1000, 1, "0 to 1000 milliseconds");
    luaL_checktype(L, 2, LUA_TTABLE);
    jetski_pump((unsigned)ms, &st);

    flag(L, "started", st.happened & JETSKI_STARTED);
    flag(L, "committed", st.happened & JETSKI_COMMITTED);
    flag(L, "finished", st.happened & JETSKI_FINISHED);
    flag(L, "failed", st.happened & JETSKI_FAILED);
    flag(L, "picture", st.happened & JETSKI_PICTURE);
    flag(L, "crashed", st.happened & JETSKI_CRASHED);
    flag(L, "loading", st.loading);
    flag(L, "can_back", st.can_back);
    flag(L, "can_forward", st.can_forward);
    lua_pushinteger(L, st.error);
    lua_setfield(L, 2, "error");
    lua_pushnumber(L, st.progress);
    lua_setfield(L, 2, "progress");
    lua_pushstring(L, st.title);
    lua_setfield(L, 2, "title");
    lua_pushstring(L, st.address);
    lua_setfield(L, 2, "address");
    lua_settop(L, 2);
    return 1;
}

/* The page into the surface, cut to it: Lua says where and how much, and
 * the arithmetic of where that is in memory is done here. */
static int l_paint(lua_State *L)
{
    unsigned sw, sh, pitch;
    uint32_t *pixels = kosmos_surface_pixels(L, 1, &sw, &sh, &pitch);
    lua_Integer x = luaL_checkinteger(L, 2), y = luaL_checkinteger(L, 3);
    lua_Integer w = luaL_checkinteger(L, 4), h = luaL_checkinteger(L, 5);

    luaL_argcheck(L, x >= 0 && (lua_Unsigned)x < sw, 2, "inside the surface");
    luaL_argcheck(L, y >= 0 && (lua_Unsigned)y < sh, 3, "inside the surface");
    if (w > (lua_Integer)sw - x) w = (lua_Integer)sw - x;
    if (h > (lua_Integer)sh - y) h = (lua_Integer)sh - y;
    if (w <= 0 || h <= 0) return 0;

    jetski_paint((uint32_t *)((uint8_t *)pixels + (size_t)y * pitch) + x,
                 (unsigned)w, (unsigned)h, pitch);
    return 0;
}

/*
 * `mouse(action, button, x, y [, mods])`: "press", "release" or "move";
 * "left", "middle", "right", or nil for a move with nothing held. In the
 * page's pixels, from its top left.
 */
static int l_mouse(lua_State *L)
{
    static const char *const actions[] = { "press", "release", "move", NULL };
    static const char *const buttons[] = { "left", "middle", "right", NULL };
    int action = luaL_checkoption(L, 1, NULL, actions);
    int button = lua_isnoneornil(L, 2) ? JETSKI_NONE : luaL_checkoption(L, 2, NULL, buttons);

    jetski_mouse((enum jetski_mouse)action, (enum jetski_button)button,
                 (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4),
                 (unsigned)(luaL_optinteger(L, 5, 0) & 15));
    return 0;
}

/* `wheel(x, y, notches [, mods])`: notches away from the person. */
static int l_wheel(lua_State *L)
{
    lua_Integer n = luaL_checkinteger(L, 3);

    if (n < -100) n = -100;
    if (n > 100) n = 100;
    jetski_wheel((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2), (int)n,
                 (unsigned)(luaL_optinteger(L, 4, 0) & 15));
    return 0;
}

/* `key(code)`, as `keys.lua` numbers a key; whether WebKit was given it. */
static int l_key(lua_State *L)
{
    lua_Integer c = luaL_checkinteger(L, 1);

    lua_pushboolean(L, c >= -1024 * 16 && c <= 255 && jetski_key((int)c));
    return 1;
}

static int l_focus(lua_State *L)
{
    jetski_focus(lua_toboolean(L, 1));
    return 0;
}

void kosmos_jetski_kit(lua_State *L)
{
    static const luaL_Reg fns[] = {
        { "open", l_open },     { "resize", l_resize }, { "load", l_load },
        { "back", l_back },     { "forward", l_forward }, { "reload", l_reload },
        { "stop", l_stop },     { "pump", l_pump },     { "paint", l_paint },
        { "mouse", l_mouse },   { "wheel", l_wheel },   { "key", l_key },
        { "focus", l_focus },   { "frame", l_frame },
        { NULL, NULL },
    };

    luaL_newlib(L, fns);
}
