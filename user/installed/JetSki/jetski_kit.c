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
 *   crash(id)                    why tab `id`'s page stopped, or nil
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
    flag(L, "asked", st.happened & JETSKI_ASKED);
    flag(L, "tabs", st.happened & JETSKI_TABS);
    flag(L, "inspector", st.happened & JETSKI_INSPECTOR);
    lua_pushinteger(L, st.tab);
    lua_setfield(L, 2, "tab");
    lua_pushinteger(L, st.opened);
    lua_setfield(L, 2, "opened");
    lua_pushinteger(L, st.closing);
    lua_setfield(L, 2, "closing");
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
    lua_pushstring(L, st.going);
    lua_setfield(L, 2, "going");
    lua_settop(L, 2);
    return 1;
}

/* web.crash(id): why tab `id`'s page stopped, as a table, or nil when it
 * did not. `known` is false until JetSki has collected the process, and
 * the fault's fields are there only once it is and a fault ended it. */
static int l_crash(lua_State *L)
{
    struct jetski_crash c;

    if (!jetski_crash((int)luaL_checkinteger(L, 1), &c)) {
        lua_pushnil(L);
        return 1;
    }
    c.fault[sizeof c.fault - 1] = '\0';
    c.address[sizeof c.address - 1] = '\0';
    lua_createtable(L, 0, 12);
    lua_pushinteger(L, c.pid);
    lua_setfield(L, -2, "pid");
    lua_pushboolean(L, c.known);
    lua_setfield(L, -2, "known");
    lua_pushstring(L, c.address);
    lua_setfield(L, -2, "address");
    if (c.known) {
        lua_pushinteger(L, c.code);
        lua_setfield(L, -2, "code");
    }
    if (c.known && c.faulted) {
        lua_pushstring(L, c.fault);
        lua_setfield(L, -2, "fault");
        lua_pushinteger(L, c.kind);
        lua_setfield(L, -2, "kind");
        lua_pushinteger(L, c.thread);
        lua_setfield(L, -2, "thread");
        lua_pushinteger(L, (lua_Integer)c.pc);
        lua_setfield(L, -2, "pc");
        lua_pushinteger(L, (lua_Integer)c.addr);
        lua_setfield(L, -2, "addr");
        lua_pushinteger(L, (lua_Integer)c.sp);
        lua_setfield(L, -2, "sp");
        lua_pushinteger(L, (lua_Integer)c.detail);
        lua_setfield(L, -2, "detail");
    }
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

/* `tab_new()`: a tab, its id; not shown until `tab_select`. */
static int l_tab_new(lua_State *L)
{
    int id = jetski_tab_new();

    if (!id) {
        lua_pushnil(L);
        lua_pushstring(L, "no tab could be made");
        return 2;
    }
    lua_pushinteger(L, id);
    return 1;
}

static int l_tab_select(lua_State *L)
{
    lua_pushboolean(L, jetski_tab_select((int)luaL_checkinteger(L, 1)));
    return 1;
}

/* `tab_close(id)`: false for the last one, which is the window's to close. */
static int l_tab_close(lua_State *L)
{
    lua_pushboolean(L, jetski_tab_close((int)luaL_checkinteger(L, 1)));
    return 1;
}

/* `tabs(list)`: the tabs in order, into `list`, reused - `{ id, shown,
 * loading, pid, title }` each - and how many. A tab's table is kept between
 * calls, so a strip drawn every pass makes no garbage once it is full. */
static int l_tabs(lua_State *L)
{
    static struct jetski_tab tabs[64];
    unsigned n = jetski_tabs(tabs, 64), i;

    luaL_checktype(L, 1, LUA_TTABLE);
    for (i = 0; i < n; i++) {
        if (lua_rawgeti(L, 1, (lua_Integer)i + 1) != LUA_TTABLE) {
            lua_pop(L, 1);
            lua_createtable(L, 0, 4);
            lua_pushvalue(L, -1);
            lua_rawseti(L, 1, (lua_Integer)i + 1);
        }
        lua_pushinteger(L, tabs[i].id);
        lua_setfield(L, -2, "id");
        lua_pushboolean(L, tabs[i].shown);
        lua_setfield(L, -2, "shown");
        lua_pushboolean(L, tabs[i].loading);
        lua_setfield(L, -2, "loading");
        lua_pushinteger(L, tabs[i].pid);
        lua_setfield(L, -2, "pid");
        lua_pushstring(L, tabs[i].title);
        lua_setfield(L, -2, "title");
        lua_pop(L, 1);
    }
    for (i = n + 1; lua_rawgeti(L, 1, (lua_Integer)i) != LUA_TNIL; i++) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_rawseti(L, 1, (lua_Integer)i);
    }
    lua_pop(L, 1);
    lua_pushinteger(L, n);
    return 1;
}

static int l_focus(lua_State *L)
{
    jetski_focus(lua_toboolean(L, 1));
    return 0;
}

/*
 * **Web Inspector** (`docs/webkit.html`, *Developer mode*):
 * `developer(on)`, `inspector_files(folder)`, `inspect(show)`,
 * `inspecting()`, `inspector_size(w, h)`, `paint_inspector(surface, x, y,
 * w, h)` as `paint` is, and `target(inspector)` - where the pointer, the
 * wheel and keys go.
 */
static int l_developer(lua_State *L)
{
    jetski_developer(lua_toboolean(L, 1));
    return 0;
}

static int l_inspector_files(lua_State *L)
{
    jetski_inspector_files(luaL_checkstring(L, 1));
    return 0;
}

static int l_inspect(lua_State *L)
{
    jetski_inspect(lua_toboolean(L, 1));
    return 0;
}

static int l_inspecting(lua_State *L)
{
    lua_pushboolean(L, jetski_inspecting());
    return 1;
}

static int l_inspector_size(lua_State *L)
{
    lua_Integer w = luaL_checkinteger(L, 1), h = luaL_checkinteger(L, 2);
    if (w > 0 && h > 0)
        jetski_inspector_size((unsigned)w, (unsigned)h);
    return 0;
}

static int l_paint_inspector(lua_State *L)
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

    jetski_paint_inspector((uint32_t *)((uint8_t *)pixels + (size_t)y * pitch) + x,
                           (unsigned)w, (unsigned)h, pitch);
    return 0;
}

static int l_target(lua_State *L)
{
    jetski_target(lua_toboolean(L, 1));
    return 0;
}

void kosmos_jetski_kit(lua_State *L)
{
    static const luaL_Reg fns[] = {
        { "open", l_open },     { "resize", l_resize }, { "load", l_load },
        { "back", l_back },     { "forward", l_forward }, { "reload", l_reload },
        { "stop", l_stop },     { "pump", l_pump },     { "paint", l_paint },
        { "crash", l_crash },
        { "mouse", l_mouse },   { "wheel", l_wheel },   { "key", l_key },
        { "focus", l_focus },   { "frame", l_frame },
        { "tab_new", l_tab_new }, { "tab_select", l_tab_select },
        { "tab_close", l_tab_close }, { "tabs", l_tabs },
        { "developer", l_developer }, { "inspector_files", l_inspector_files },
        { "inspect", l_inspect }, { "inspecting", l_inspecting },
        { "inspector_size", l_inspector_size }, { "paint_inspector", l_paint_inspector },
        { "target", l_target },
        { NULL, NULL },
    };

    luaL_newlib(L, fns);
}
