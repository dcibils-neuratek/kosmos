/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The WebKit Kit's door from Lua** (`webkit.h`, `docs/mailweb.md` W2):
 * `use("/Kosmos/Kits/webkit")`, from a program running in Kosmos's WebKit
 * image (`-- kosmos: image webkit`). Elsewhere there is no kit by that
 * name, since only `webkit.elf` links it.
 *
 *   webkit.view(w, h [, {scripts, follow}])   a view, dark; or nil and why
 *   webkit.pump(ms, into)        WebKit for at most `ms`, or until a view
 *                                shown has news: into `into`, opened,
 *                                opener, closing and others
 *   webkit.views(list)           every view, `{ id, shown, loading, pid,
 *                                title }`, into `list`; how many
 *   webkit.frame(x, y, w, h)     where the window is, and its whole size
 *   webkit.developer(on), webkit.inspector_files(folder),
 *   webkit.inspector_size(w, h)
 *
 * and a view's own:
 *
 *   v:id()  v:show(on)  v:close()  v:resize(w, h)
 *   v:load(address)  v:back()  v:forward()  v:reload()  v:stop()
 *   v:state(into)                what happened since it was last asked,
 *                                and how it stands, into `into`
 *   v:crash()                    why its page stopped, or nil
 *   v:paint(surface, x, y, w, h)
 *   v:mouse(action, button, x, y [, mods])  v:wheel(x, y, notches [, mods])
 *   v:key(code)  v:focus(on)
 *   v:inspect(show)  v:inspecting()  v:paint_inspector(surface, x, y, w, h)
 *   v:target(inspector)
 *   v:save(path, complete)  v:saved()   Save page as; `saved` in v:state
 *
 * `pump` and `state` fill tables the caller keeps rather than making them:
 * they run on every pass of the window's loop, and a table a pass is
 * garbage the collector has to come for in the middle of a frame.
 *
 * Until W2 this was `jetski_kit.c`, one page with tabs, as `use("jetski.elf")`.
 */

#include "lua.h"
#include "lauxlib.h"

#include "webkit.h"
#include <kosmos.h>
#include "kits/gfx/gfx_draw.h"      /* kosmos_surface_pixels */
#include "kits/kitenv.h"            /* kit_capability, kit_push_spill */

/* `posix_program.h`: where this process's standard error goes, and so
 * the web and network processes' it starts. */
void posix_console(long capability);
long posix_console_capability(void);

#define VIEW "kosmos.webkit.view"

struct view { int id; };

static unsigned side(lua_State *L, int index)
{
    lua_Integer v = luaL_checkinteger(L, index);

    luaL_argcheck(L, v > 0 && v <= 16384, index, "1 to 16384");
    return (unsigned)v;
}

/* A view's id; an error for one closed. */
static int id_of(lua_State *L)
{
    struct view *v = luaL_checkudata(L, 1, VIEW);

    luaL_argcheck(L, v->id != 0, 1, "a view that was closed");
    return v->id;
}

static int option(lua_State *L, int table, const char *name, int otherwise)
{
    int on = otherwise;

    if (lua_getfield(L, table, name) != LUA_TNIL)
        on = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return on;
}

static int l_view(lua_State *L)
{
    unsigned w = side(L, 1), h = side(L, 2);
    struct webkit_options options = { 1, 1 };
    struct view *v;
    int id;

    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        options.scripts = option(L, 3, "scripts", 1);
        options.follow = option(L, 3, "follow", 1);
    }
    if (posix_console_capability() < 0)
        posix_console(kit_capability(L, "/Devices/console"));

    id = webkit_view(w, h, &options);
    if (!id) {
        lua_pushnil(L);
        lua_pushliteral(L, "the view was not made");
        return 2;
    }
    v = lua_newuserdatauv(L, sizeof *v, 0);
    v->id = id;
    luaL_setmetatable(L, VIEW);
    return 1;
}

/*
 * A view WebKit made - one a page opened (`pump`'s `opened`) - as the
 * caller's: `webkit.adopt(id)`. Its options are its opener's.
 */
static int l_adopt(lua_State *L)
{
    lua_Integer id = luaL_checkinteger(L, 1);
    struct view *v;

    luaL_argcheck(L, id > 0 && id <= 0x7fffffff, 1, "a view's id");
    v = lua_newuserdatauv(L, sizeof *v, 0);
    v->id = (int)id;
    luaL_setmetatable(L, VIEW);
    return 1;
}

static void flag(lua_State *L, int table, const char *name, int on)
{
    lua_pushboolean(L, on);
    lua_setfield(L, table, name);
}

static int l_pump(lua_State *L)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    struct webkit_events ev;

    luaL_argcheck(L, ms >= 0 && ms <= 1000, 1, "0 to 1000 milliseconds");
    luaL_checktype(L, 2, LUA_TTABLE);
    webkit_pump((unsigned)ms, &ev);
    lua_pushinteger(L, ev.opened);
    lua_setfield(L, 2, "opened");
    lua_pushinteger(L, ev.opener);
    lua_setfield(L, 2, "opener");
    lua_pushinteger(L, ev.closing);
    lua_setfield(L, 2, "closing");
    flag(L, 2, "others", ev.others);
    flag(L, 2, "downloads", ev.downloads);
    lua_pushinteger(L, ev.downloading);
    lua_setfield(L, 2, "downloading");
    lua_settop(L, 2);
    return 1;
}

/* `downloads(list)`: every download, the newest first, into `list`,
 * reused as `views` is; how many. Each: id, view, state ("downloading",
 * "done", "failed", "cancelled"), received, expected (nil when unsaid),
 * seconds, error, name, path, address, reason. */
static int l_downloads(lua_State *L)
{
    static struct webkit_download info[64];
    static const char *states[] = { "downloading", "done", "failed", "cancelled" };
    unsigned n = webkit_downloads(info, 64), i;

    luaL_checktype(L, 1, LUA_TTABLE);
    for (i = 0; i < n; i++) {
        if (lua_rawgeti(L, 1, (lua_Integer)i + 1) != LUA_TTABLE) {
            lua_pop(L, 1);
            lua_createtable(L, 0, 12);
            lua_pushvalue(L, -1);
            lua_rawseti(L, 1, (lua_Integer)i + 1);
        }
        lua_pushinteger(L, info[i].id);
        lua_setfield(L, -2, "id");
        lua_pushinteger(L, info[i].view);
        lua_setfield(L, -2, "view");
        lua_pushstring(L, states[info[i].state & 3]);
        lua_setfield(L, -2, "state");
        lua_pushinteger(L, info[i].received);
        lua_setfield(L, -2, "received");
        if (info[i].expected >= 0)
            lua_pushinteger(L, info[i].expected);
        else
            lua_pushnil(L);
        lua_setfield(L, -2, "expected");
        lua_pushnumber(L, info[i].seconds);
        lua_setfield(L, -2, "seconds");
        lua_pushinteger(L, info[i].error);
        lua_setfield(L, -2, "error");
        lua_pushstring(L, info[i].name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, info[i].path);
        lua_setfield(L, -2, "path");
        lua_pushstring(L, info[i].address);
        lua_setfield(L, -2, "address");
        lua_pushstring(L, info[i].reason);
        lua_setfield(L, -2, "reason");
        lua_pop(L, 1);
    }
    /* Rows past the end, from a longer list before, go. */
    for (i = n + 1; lua_rawgeti(L, 1, (lua_Integer)i) != LUA_TNIL; i++) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_rawseti(L, 1, (lua_Integer)i);
    }
    lua_pop(L, 1);
    lua_pushinteger(L, n);
    return 1;
}

static int l_download_cancel(lua_State *L)
{
    lua_pushboolean(L, webkit_download_cancel((int)luaL_checkinteger(L, 1)));
    return 1;
}

static int l_download_forget(lua_State *L)
{
    webkit_download_forget((int)luaL_checkinteger(L, 1));
    return 0;
}

/* `views(list)`: every view in order, into `list`, reused - a view's table
 * is kept between calls, so a strip drawn every pass makes no garbage once
 * it is full. */
static int l_views(lua_State *L)
{
    static struct webkit_view_info info[64];
    unsigned n = webkit_views(info, 64), i;

    luaL_checktype(L, 1, LUA_TTABLE);
    for (i = 0; i < n; i++) {
        if (lua_rawgeti(L, 1, (lua_Integer)i + 1) != LUA_TTABLE) {
            lua_pop(L, 1);
            lua_createtable(L, 0, 6);
            lua_pushvalue(L, -1);
            lua_rawseti(L, 1, (lua_Integer)i + 1);
        }
        lua_pushinteger(L, info[i].id);
        lua_setfield(L, -2, "id");
        lua_pushboolean(L, info[i].shown);
        lua_setfield(L, -2, "shown");
        lua_pushboolean(L, info[i].loading);
        lua_setfield(L, -2, "loading");
        lua_pushinteger(L, info[i].pid);
        lua_setfield(L, -2, "pid");
        lua_pushstring(L, info[i].title);
        lua_setfield(L, -2, "title");
        lua_pushstring(L, info[i].address);
        lua_setfield(L, -2, "address");
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

static int l_frame(lua_State *L)
{
    webkit_frame((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2),
                 side(L, 3), side(L, 4));
    return 0;
}

static int l_developer(lua_State *L)
{
    webkit_developer(lua_toboolean(L, 1));
    return 0;
}

/* webkit.settings{ scripts = true, images = true }: every view's. */
static int l_settings(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    webkit_settings(option(L, 1, "scripts", 1), option(L, 1, "images", 1));
    return 0;
}

/* webkit.zoom(125): every page's, in per cent. */
static int l_zoom(lua_State *L)
{
    lua_Integer percent = luaL_checkinteger(L, 1);
    luaL_argcheck(L, percent >= 25 && percent <= 500, 1, "a zoom from 25 to 500 per cent");
    webkit_zoom((int)percent);
    return 0;
}

/* webkit.wake(door, code) - the window manager's door and the window's
 * code from `watch_code`: WebKit's news then wakes the window's poll. */
static int l_wake(lua_State *L)
{
    webkit_wake((long)luaL_checkinteger(L, 1), (unsigned)luaL_checkinteger(L, 2));
    return 0;
}

/* webkit.store_forgets(true): a suite's control only (`webkit.h`). */
static int l_store_forgets(lua_State *L)
{
    webkit_store_forgets(lua_toboolean(L, 1));
    return 0;
}

static int l_trace(lua_State *L)
{
    webkit_trace(lua_toboolean(L, 1));
    return 0;
}

/* webkit.said() - what the kit wrote since it was last asked, its trace's
 * lines among it, or "". The kit's standard error is the spill ring in a
 * process that does not own the console, and only QEMU's serial line
 * showed it: on a machine, its program says it into the log. */
static int l_said(lua_State *L)
{
    kit_push_spill(L);
    return 1;
}

static int l_inspector_files(lua_State *L)
{
    webkit_inspector_files(luaL_checkstring(L, 1));
    return 0;
}

static int l_inspector_size(lua_State *L)
{
    lua_Integer w = luaL_checkinteger(L, 1), h = luaL_checkinteger(L, 2);

    if (w > 0 && h > 0)
        webkit_inspector_size((unsigned)w, (unsigned)h);
    return 0;
}

/* ---- a view's own ---- */

static int v_id(lua_State *L)
{
    struct view *v = luaL_checkudata(L, 1, VIEW);

    lua_pushinteger(L, v->id);
    return 1;
}

static int v_show(lua_State *L)
{
    webkit_show(id_of(L), lua_toboolean(L, 2));
    return 0;
}

static int v_close(lua_State *L)
{
    struct view *v = luaL_checkudata(L, 1, VIEW);

    if (v->id)
        webkit_close(v->id);
    v->id = 0;
    return 0;
}

static int v_resize(lua_State *L)
{
    int id = id_of(L);

    webkit_resize(id, side(L, 2), side(L, 3));
    return 0;
}

static int v_load(lua_State *L)
{
    int id = id_of(L);

    webkit_load(id, luaL_checkstring(L, 2));
    return 0;
}

static int v_back(lua_State *L)    { webkit_back(id_of(L)); return 0; }
static int v_forward(lua_State *L) { webkit_forward(id_of(L)); return 0; }
static int v_reload(lua_State *L)  { webkit_reload(id_of(L)); return 0; }
static int v_stop(lua_State *L)    { webkit_stop(id_of(L)); return 0; }

static int v_state(lua_State *L)
{
    int id = id_of(L);
    struct webkit_state st;

    luaL_checktype(L, 2, LUA_TTABLE);
    if (!webkit_state(id, &st)) {
        lua_pushnil(L);
        return 1;
    }
    flag(L, 2, "started", st.happened & WEBKIT_STARTED);
    flag(L, 2, "committed", st.happened & WEBKIT_COMMITTED);
    flag(L, 2, "finished", st.happened & WEBKIT_FINISHED);
    flag(L, 2, "failed", st.happened & WEBKIT_FAILED);
    flag(L, 2, "picture", st.happened & WEBKIT_PICTURE);
    flag(L, 2, "crashed", st.happened & WEBKIT_CRASHED);
    flag(L, 2, "asked", st.happened & WEBKIT_ASKED);
    flag(L, 2, "inspector", st.happened & WEBKIT_INSPECTOR);
    flag(L, 2, "hover", st.happened & WEBKIT_HOVER);
    flag(L, 2, "found", st.happened & WEBKIT_FOUND);
    flag(L, 2, "saved", st.happened & WEBKIT_SAVED);
    flag(L, 2, "loading", st.loading);
    flag(L, 2, "can_back", st.can_back);
    flag(L, 2, "can_forward", st.can_forward);
    lua_pushinteger(L, st.error);
    lua_setfield(L, 2, "error");
    lua_pushinteger(L, st.blocked);
    lua_setfield(L, 2, "blocked");
    lua_pushinteger(L, st.cookies_blocked);
    lua_setfield(L, 2, "cookies_blocked");
    lua_pushnumber(L, st.progress);
    lua_setfield(L, 2, "progress");
    lua_pushstring(L, st.title);
    lua_setfield(L, 2, "title");
    lua_pushstring(L, st.address);
    lua_setfield(L, 2, "address");
    lua_pushstring(L, st.going);
    lua_setfield(L, 2, "going");
    flag(L, 2, "unblocked", st.unblocked);
    flag(L, 2, "refused", st.refused);
    if (st.refused) {
        lua_pushinteger(L, st.certificate_error);
        lua_setfield(L, 2, "certificate_error");
        lua_pushstring(L, st.refused_reason);
        lua_setfield(L, 2, "refused_reason");
        lua_pushstring(L, st.refused_address);
        lua_setfield(L, 2, "refused_address");
    }
    lua_settop(L, 2);
    return 1;
}

/* `v:security()`: how its page came (`docs/padlock.html`) - `came` one of
 * "secure", "plain", "mixed", "anyway", "file" or "none" - and, when the
 * network process handed it on, the certificate: `subject`, `issuer`,
 * `names`, `valid_from` and `valid_until` in seconds since 1970, `error`,
 * and `chain`, how many the server sent. */
static int v_security(lua_State *L)
{
    static const char *const words[] = { "none", "secure", "plain", "mixed", "anyway", "file" };
    struct webkit_security sec;

    if (!webkit_security(id_of(L), &sec)) {
        lua_pushnil(L);
        return 1;
    }
    sec.subject[sizeof sec.subject - 1] = '\0';
    sec.issuer[sizeof sec.issuer - 1] = '\0';
    sec.names[sizeof sec.names - 1] = '\0';
    lua_createtable(L, 0, 8);
    lua_pushstring(L, sec.came >= 0 && sec.came <= WEBKIT_CAME_FILE ? words[sec.came] : "none");
    lua_setfield(L, -2, "came");
    lua_pushinteger(L, sec.chain);
    lua_setfield(L, -2, "chain");
    lua_pushinteger(L, sec.error);
    lua_setfield(L, -2, "error");
    if (sec.chain) {
        lua_pushstring(L, sec.subject);
        lua_setfield(L, -2, "subject");
        lua_pushstring(L, sec.issuer);
        lua_setfield(L, -2, "issuer");
        lua_pushstring(L, sec.names);
        lua_setfield(L, -2, "names");
        lua_pushinteger(L, (lua_Integer)sec.valid_from);
        lua_setfield(L, -2, "valid_from");
        lua_pushinteger(L, (lua_Integer)sec.valid_until);
        lua_setfield(L, -2, "valid_until");
    }
    return 1;
}

/* `v:kept([room])`: what was kept out of its page, by host, the most
 * first - a list of { host =, count = } - and how many hosts in all. */
static int v_kept(lua_State *L)
{
    struct webkit_kept list[16];
    lua_Integer room = luaL_optinteger(L, 2, 16);
    if (room < 0) room = 0;
    if (room > 16) room = 16;
    unsigned all = webkit_kept(id_of(L), list, (unsigned)room);
    unsigned n = all < (unsigned)room ? all : (unsigned)room;

    lua_createtable(L, (int)n, 0);
    for (unsigned i = 0; i < n; i++) {
        list[i].host[sizeof list[i].host - 1] = '\0';
        lua_createtable(L, 0, 2);
        lua_pushstring(L, list[i].host);
        lua_setfield(L, -2, "host");
        lua_pushinteger(L, list[i].count);
        lua_setfield(L, -2, "count");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_pushinteger(L, all);
    return 2;
}

/* `webkit.allow(list)`: the sites allowed past blocking, one a line. */
static int l_allow(lua_State *L)
{
    webkit_allow_sites(luaL_checkstring(L, 1));
    return 0;
}

/* `v:anyway()`: its refused page loaded, its certificate taken as it is,
 * for this view alone; false when nothing was refused. */
static int v_anyway(lua_State *L)
{
    lua_pushboolean(L, webkit_anyway(id_of(L)));
    return 1;
}

/* `v:crash()`: why its page stopped, as a table, or nil when it did not.
 * `known` is false until the kit has collected the process, and the
 * fault's fields are there only once it is and a fault ended it. */
static int v_crash(lua_State *L)
{
    struct webkit_crash c;

    if (!webkit_crash(id_of(L), &c)) {
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

/* The page, or its inspector, into the surface, cut to it: Lua says where
 * and how much, and the arithmetic of where that is in memory is done here. */
static int paint_with(lua_State *L,
                      void (*paint)(int, uint32_t *, unsigned, unsigned, unsigned))
{
    int id = id_of(L);
    unsigned sw, sh, pitch;
    uint32_t *pixels = kosmos_surface_pixels(L, 2, &sw, &sh, &pitch);
    lua_Integer x = luaL_checkinteger(L, 3), y = luaL_checkinteger(L, 4);
    lua_Integer w = luaL_checkinteger(L, 5), h = luaL_checkinteger(L, 6);

    luaL_argcheck(L, x >= 0 && (lua_Unsigned)x < sw, 3, "inside the surface");
    luaL_argcheck(L, y >= 0 && (lua_Unsigned)y < sh, 4, "inside the surface");
    if (w > (lua_Integer)sw - x) w = (lua_Integer)sw - x;
    if (h > (lua_Integer)sh - y) h = (lua_Integer)sh - y;
    if (w <= 0 || h <= 0) return 0;

    paint(id, (uint32_t *)((uint8_t *)pixels + (size_t)y * pitch) + x,
          (unsigned)w, (unsigned)h, pitch);
    return 0;
}

/*
 * `v:paint(s, x, y, w, h [, px, py, pw, ph])`: the page, `w` by `h`, at
 * `x`, `y` in the surface - all of it, or only the part `px`, `py`, `pw`,
 * `ph` of it in the page's own pixels, the rest of the surface untouched.
 */
static void paint_whole(int id, uint32_t *pixels, unsigned w, unsigned h, unsigned pitch)
{
    webkit_paint(id, pixels, w, h, pitch, 0, 0, (int)w, (int)h);
}

static int v_paint(lua_State *L)
{
    if (lua_isnoneornil(L, 7))
        return paint_with(L, paint_whole);

    int id = id_of(L);
    unsigned sw, sh, pitch;
    uint32_t *pixels = kosmos_surface_pixels(L, 2, &sw, &sh, &pitch);
    lua_Integer x = luaL_checkinteger(L, 3), y = luaL_checkinteger(L, 4);
    lua_Integer w = luaL_checkinteger(L, 5), h = luaL_checkinteger(L, 6);
    lua_Integer px = luaL_checkinteger(L, 7), py = luaL_checkinteger(L, 8);
    lua_Integer pw = luaL_checkinteger(L, 9), ph = luaL_checkinteger(L, 10);

    luaL_argcheck(L, x >= 0 && (lua_Unsigned)x < sw, 3, "inside the surface");
    luaL_argcheck(L, y >= 0 && (lua_Unsigned)y < sh, 4, "inside the surface");
    if (w > (lua_Integer)sw - x) w = (lua_Integer)sw - x;
    if (h > (lua_Integer)sh - y) h = (lua_Integer)sh - y;
    // The part, cut to the page: nothing outside it is ever drawn.
    if (px < 0) { pw += px; px = 0; }
    if (py < 0) { ph += py; py = 0; }
    if (pw > w - px) pw = w - px;
    if (ph > h - py) ph = h - py;
    if (w <= 0 || h <= 0 || pw <= 0 || ph <= 0) return 0;

    webkit_paint(id, (uint32_t *)((uint8_t *)pixels + (size_t)y * pitch) + x,
                 (unsigned)w, (unsigned)h, pitch, (int)px, (int)py, (int)pw, (int)ph);
    return 0;
}

static int v_paint_inspector(lua_State *L) { return paint_with(L, webkit_paint_inspector); }

/* `v:damage()`: x, y, w, h of what changed in the page's picture since
 * this was last asked, in the page's pixels, or nil when nothing did. */
static int v_damage(lua_State *L)
{
    int x, y, w, h;

    if (!webkit_damage(id_of(L), &x, &y, &w, &h)) return 0;
    lua_pushinteger(L, x);
    lua_pushinteger(L, y);
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 4;
}

/*
 * `v:shown([generation [, everything]])` (`webkit.md` *Fast* 1c): when the page changed
 * since this was last asked, the buffer just finished, 1 or 2, and what
 * changed in it, `x`, `y`, `w`, `h`, then its store's `generation`, width
 * and height - and, when that generation is not the one given, a
 * capability to the store's region that can only read it, for the window
 * manager, which the caller lets go once it is handed over. Nothing when
 * nothing changed, or when the store is not in a region. `everything`:
 * the whole buffer said to have changed, for a window manager not shown it.
 */
static int v_shown(lua_State *L)
{
    struct webkit_shown out;
    lua_Integer known = luaL_optinteger(L, 2, -1);

    if (!webkit_shown(id_of(L), lua_toboolean(L, 3), &out)) return 0;
    lua_pushinteger(L, out.buffer);
    lua_pushinteger(L, out.x);
    lua_pushinteger(L, out.y);
    lua_pushinteger(L, out.w);
    lua_pushinteger(L, out.h);
    lua_pushinteger(L, out.generation);
    lua_pushinteger(L, out.width);
    lua_pushinteger(L, out.height);

    if ((lua_Integer)out.generation != known) {
        long ro = kosmos_mem_readonly(out.region);

        if (ro >= 0) {
            lua_pushinteger(L, ro);
            return 9;
        }
    }

    return 8;
}

/*
 * `v:mouse(action, button, x, y [, mods])`: "press", "release" or "move";
 * "left", "middle", "right", or nil for a move with nothing held. In the
 * view's pixels, from its top left.
 */
static int v_mouse(lua_State *L)
{
    static const char *const actions[] = { "press", "release", "move", NULL };
    static const char *const buttons[] = { "left", "middle", "right", NULL };
    int id = id_of(L);
    int action = luaL_checkoption(L, 2, NULL, actions);
    int button = lua_isnoneornil(L, 3) ? WEBKIT_NONE : luaL_checkoption(L, 3, NULL, buttons);

    webkit_mouse(id, (enum webkit_mouse)action, (enum webkit_button)button,
                 (int)luaL_checkinteger(L, 4), (int)luaL_checkinteger(L, 5),
                 (unsigned)(luaL_optinteger(L, 6, 0) & 15));
    return 0;
}

/* `v:wheel(x, y, notches [, mods])`: notches away from the person. */
static int v_wheel(lua_State *L)
{
    int id = id_of(L);
    lua_Integer n = luaL_checkinteger(L, 4);

    if (n < -100) n = -100;
    if (n > 100) n = 100;
    webkit_wheel(id, (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3), (int)n,
                 (unsigned)(luaL_optinteger(L, 5, 0) & 15));
    return 0;
}

/* `v:key(code)`, as `keys.lua` numbers a key; whether WebKit was given it. */
static int v_key(lua_State *L)
{
    int id = id_of(L);
    lua_Integer c = luaL_checkinteger(L, 2);

    lua_pushboolean(L, c >= -1024 * 16 && c <= 255 && webkit_key(id, (int)c));
    return 1;
}

static int v_focus(lua_State *L)
{
    webkit_focus(id_of(L), lua_toboolean(L, 2));
    return 0;
}

static int v_inspect(lua_State *L)
{
    webkit_inspect(id_of(L), lua_toboolean(L, 2));
    return 0;
}

static int v_inspecting(lua_State *L)
{
    lua_pushboolean(L, webkit_inspecting(id_of(L)));
    return 1;
}

/* `v:under()`: what is under the pointer - `{ link, label, image,
 * editable, selected }`, the strings "" where there is none. */
static int v_under(lua_State *L)
{
    static struct webkit_under u;

    webkit_under(id_of(L), &u);
    lua_createtable(L, 0, 5);
    lua_pushstring(L, u.link);
    lua_setfield(L, -2, "link");
    lua_pushstring(L, u.label);
    lua_setfield(L, -2, "label");
    lua_pushstring(L, u.image);
    lua_setfield(L, -2, "image");
    lua_pushboolean(L, u.editable);
    lua_setfield(L, -2, "editable");
    lua_pushstring(L, u.selected);
    lua_setfield(L, -2, "selected");
    return 1;
}

/* `v:find(words, match_case, backwards, fresh)`: every match marked, the
 * next selected; "" takes the marks off. `v:found()`: how many (-1 past a
 * thousand) and which is shown, from 1 - as WebKit last answered. */
static int v_find(lua_State *L)
{
    int id = id_of(L);
    webkit_find(id, luaL_checkstring(L, 2), lua_toboolean(L, 3), lua_toboolean(L, 4),
                lua_toboolean(L, 5));
    return 0;
}

static int v_found(lua_State *L)
{
    int count = 0, current = 0;

    webkit_found(id_of(L), &count, &current);
    lua_pushinteger(L, count);
    lua_pushinteger(L, current);
    return 2;
}

/* v:save(path, complete) - Save page as; `saved` in `v:state` when done. */
static int v_save(lua_State *L)
{
    webkit_save(id_of(L), luaL_checkstring(L, 2), lua_toboolean(L, 3));
    return 0;
}

/* v:saved() - the bytes written, or nil and why not. */
static int v_saved(lua_State *L)
{
    char why[160];
    long long bytes = webkit_saved(id_of(L), why, sizeof why);

    if (bytes > 0) {
        lua_pushinteger(L, (lua_Integer)bytes);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, why[0] ? why : "nothing was saved");
    return 2;
}

static int v_target(lua_State *L)
{
    webkit_target(id_of(L), lua_toboolean(L, 2));
    return 0;
}

void kosmos_webkit_kit(lua_State *L)
{
    static const luaL_Reg methods[] = {
        { "id", v_id },           { "show", v_show },       { "close", v_close },
        { "resize", v_resize },   { "load", v_load },       { "back", v_back },
        { "forward", v_forward }, { "reload", v_reload },   { "stop", v_stop },
        { "state", v_state },     { "crash", v_crash },     { "paint", v_paint },
        { "mouse", v_mouse },     { "wheel", v_wheel },     { "key", v_key },
        { "focus", v_focus },     { "inspect", v_inspect }, { "inspecting", v_inspecting },
        { "paint_inspector", v_paint_inspector },           { "target", v_target },
        { "damage", v_damage },
        { "shown", v_shown },
        { "security", v_security }, { "anyway", v_anyway }, { "kept", v_kept },
        { "under", v_under }, { "find", v_find }, { "found", v_found },
        { "save", v_save }, { "saved", v_saved },
        { NULL, NULL },
    };
    static const luaL_Reg fns[] = {
        { "view", l_view },       { "adopt", l_adopt },     { "pump", l_pump },
        { "views", l_views },     { "frame", l_frame },     { "developer", l_developer },
        { "trace", l_trace },     { "wake", l_wake },
        { "store_forgets", l_store_forgets },
        { "settings", l_settings }, { "zoom", l_zoom },
        { "said", l_said },       { "allow", l_allow },
        { "downloads", l_downloads }, { "download_cancel", l_download_cancel },
        { "download_forget", l_download_forget },
        { "inspector_files", l_inspector_files },           { "inspector_size", l_inspector_size },
        { NULL, NULL },
    };

    if (luaL_newmetatable(L, VIEW)) {
        luaL_newlib(L, methods);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
    luaL_newlib(L, fns);
}
