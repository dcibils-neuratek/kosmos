/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The scene: the desktop composed in C (`docs/astra-display.md`, D3a).
 *
 *   local sc = gfx.scene(back)
 *   sc:look(corner, shadow, grip, edge_light, edge_dark, box, box_w, run, d,
 *           track, line_soft, outline, lights...)       what it looks like
 *   sc:desktop(colour, wallpaper, wx, wy, ww, wh, stamp, sx, sy)
 *   sc:windows(n)                                       how many, in order
 *   sc:window(k, x, y, w, h, flags, picture, src_w, src_h,
 *             bar, bar_h, bx, by, held)                 the k-th from the back
 *   sc:menus(n)  sc:menu(k, x, y, w, h, picture)        above every window
 *   sc:above(outline x, y, w, h, colour | nil, osd, ox, oy, alpha | nil,
 *            cursor, cx, cy | nil)                      over everything
 *   sc:compose(x, y, w, h)  -> pixels drawn             one damaged rectangle
 *
 * **It decides nothing a person would choose.** The window manager says
 * where every window is, what stacks over what, what each looks like and
 * which picture shows it, and redescribes the scene before each pass; this
 * file does what `compose.lua` and `drawwindow.lua` did with that, per
 * damaged rectangle, into the back buffer: which windows show and which
 * are hidden behind others, the desktop under them, each window's shadow,
 * its picture - copied, blended or stretched - its sizing grip and its
 * three lights, its rounded corners, then the menus, the resize outline,
 * the volume display and the pointer. The screen is the caller's to put it
 * on, and a drag's label the caller's to draw.
 *
 * Compiled inside `gfx.c`, so it draws with the kit's own primitives -
 * `blit_on`, `blend_on`, `stretch_into` and the rest - and not a copy of
 * them; a surface is held by its userdata, kept alive in the scene's user
 * value for as long as the scene names it.
 */

#include "scene_core.h"

#define SCENE_MT "kosmos.scene"

/* The user value's slots: what the scene names, kept alive. */
enum { UV_WINDOWS = 1, UV_BARS, UV_MENUS, UV_MISC, UV_COUNT = UV_MISC };
enum { MISC_WALLPAPER = 1, MISC_STAMP, MISC_OSD, MISC_CURSOR };

static struct scene *check_scene(lua_State *L)
{
    return luaL_checkudata(L, 1, SCENE_MT);
}

/* A surface argument that may be nil; a freed one is as good as none. */
static struct surface *opt_surface(lua_State *L, int index)
{
    struct surface *s;

    if (lua_isnoneornil(L, index)) {
        return NULL;
    }

    s = luaL_checkudata(L, index, SURFACE_MT);
    return s->pixels != NULL ? s : NULL;
}

/* `value` at `slot`[k] of the user value, so the collector keeps it. */
static void hold(lua_State *L, int slot, lua_Integer k, int value)
{
    value = lua_absindex(L, value);
    lua_getiuservalue(L, 1, 1);
    lua_rawgeti(L, -1, slot);
    lua_pushvalue(L, value);
    lua_rawseti(L, -2, k);
    lua_pop(L, 2);
}

/* Everything past `n` in `slot` let go of. */
static void let_go(lua_State *L, int slot, int n, int had)
{
    int k;

    lua_getiuservalue(L, 1, 1);
    lua_rawgeti(L, -1, slot);

    for (k = n + 1; k <= had; k++) {
        lua_pushnil(L);
        lua_rawseti(L, -2, k);
    }

    lua_pop(L, 2);
}


/* --------------------------------------------------------------------- */
/* Being described.                                                       */
/* --------------------------------------------------------------------- */

static int l_scene(lua_State *L)
{
    struct surface *back = check_surface(L, 1);
    struct scene *sc = lua_newuserdatauv(L, sizeof(*sc), 1);
    int slot;

    scene_init(sc, back);

    lua_createtable(L, UV_COUNT, 0);

    for (slot = 1; slot <= UV_COUNT; slot++) {
        lua_newtable(L);
        lua_rawseti(L, -2, slot);
    }

    /* The back buffer, kept too. */
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "back");
    lua_setiuservalue(L, -2, 1);

    luaL_setmetatable(L, SCENE_MT);
    return 1;
}

static int l_scene_gc(lua_State *L)
{
    struct scene *sc = luaL_checkudata(L, 1, SCENE_MT);

    scene_free(sc);
    return 0;
}

static int l_scene_look(lua_State *L)
{
    struct scene *sc = check_scene(L);
    int k, at = 13;

    sc->corner     = (long)luaL_checkinteger(L, 2);
    sc->shadow     = (long)luaL_checkinteger(L, 3);
    sc->grip       = (long)luaL_checkinteger(L, 4);
    sc->edge_light = (uint32_t)luaL_checkinteger(L, 5);
    sc->edge_dark  = (uint32_t)luaL_checkinteger(L, 6);
    sc->box        = (long)luaL_checkinteger(L, 7);
    sc->box_w      = (long)luaL_checkinteger(L, 8);
    sc->run        = (long)luaL_checkinteger(L, 9);
    sc->d          = (long)luaL_checkinteger(L, 10);
    sc->track      = (uint32_t)luaL_checkinteger(L, 11);
    sc->line_soft  = (uint32_t)luaL_checkinteger(L, 12);
    sc->outline    = (long)luaL_checkinteger(L, at++);

    /* Each light by slot: its fill, ring, mark, and its fill held down. */
    for (k = 0; k < SC_SLOTS; k++) {
        sc->fill[k]      = (uint32_t)luaL_checkinteger(L, at++);
        sc->ring[k]      = (uint32_t)luaL_checkinteger(L, at++);
        sc->mark[k]      = (uint32_t)luaL_checkinteger(L, at++);
        sc->held_fill[k] = (uint32_t)luaL_checkinteger(L, at++);
    }

    return 0;
}

static int l_scene_desktop(lua_State *L)
{
    struct scene *sc = check_scene(L);

    sc->desktop   = (uint32_t)luaL_checkinteger(L, 2);
    sc->wallpaper = opt_surface(L, 3);
    sc->wx = (long)luaL_optinteger(L, 4, 0);
    sc->wy = (long)luaL_optinteger(L, 5, 0);
    sc->ww = (long)luaL_optinteger(L, 6, 0);
    sc->wh = (long)luaL_optinteger(L, 7, 0);
    sc->stamp = opt_surface(L, 8);
    sc->sx = (long)luaL_optinteger(L, 9, 0);
    sc->sy = (long)luaL_optinteger(L, 10, 0);

    hold(L, UV_MISC, MISC_WALLPAPER, 3);
    hold(L, UV_MISC, MISC_STAMP, 8);
    return 0;
}

static int l_scene_windows(lua_State *L)
{
    struct scene *sc = check_scene(L);
    int n = (int)luaL_checkinteger(L, 2);
    int had = sc->nwin;

    luaL_argcheck(L, n >= 0 && n <= 4096, 2, "a scene holds 0 to 4096 windows");
    if (!scene_set_windows(sc, n)) {
        return luaL_error(L, "no memory for a scene of %d windows", n);
    }

    let_go(L, UV_WINDOWS, n, had);
    let_go(L, UV_BARS, n, had);
    return 0;
}

static int l_scene_window(lua_State *L)
{
    struct scene *sc = check_scene(L);
    int k = (int)luaL_checkinteger(L, 2);
    struct sc_window *w;

    luaL_argcheck(L, k >= 1 && k <= sc->nwin, 2, "no such window in the scene");
    w = &sc->win[k - 1];
    w->x       = (long)luaL_checkinteger(L, 3);
    w->y       = (long)luaL_checkinteger(L, 4);
    w->w       = (long)luaL_checkinteger(L, 5);
    w->h       = (long)luaL_checkinteger(L, 6);
    w->flags   = (unsigned)luaL_checkinteger(L, 7);
    w->picture = opt_surface(L, 8);
    w->src_w   = (long)luaL_optinteger(L, 9, w->w);
    w->src_h   = (long)luaL_optinteger(L, 10, w->h);
    w->bar     = opt_surface(L, 11);
    w->bar_h   = (long)luaL_optinteger(L, 12, 0);
    w->bx      = (long)luaL_optinteger(L, 13, 0);
    w->by      = (long)luaL_optinteger(L, 14, 0);
    w->held    = (int)luaL_optinteger(L, 15, -1);

    hold(L, UV_WINDOWS, k, 8);
    hold(L, UV_BARS, k, 11);
    return 0;
}

static int l_scene_menus(lua_State *L)
{
    struct scene *sc = check_scene(L);
    int n = (int)luaL_checkinteger(L, 2);
    int had = sc->nmenu;

    luaL_argcheck(L, n >= 0 && n <= 256, 2, "a scene holds 0 to 256 menus");
    if (!scene_set_menus(sc, n)) {
        return luaL_error(L, "no memory for a scene of %d menus", n);
    }

    let_go(L, UV_MENUS, n, had);
    return 0;
}

static int l_scene_menu(lua_State *L)
{
    struct scene *sc = check_scene(L);
    int k = (int)luaL_checkinteger(L, 2);
    struct sc_menu *m;

    luaL_argcheck(L, k >= 1 && k <= sc->nmenu, 2, "no such menu in the scene");
    m = &sc->menu[k - 1];
    m->x = (long)luaL_checkinteger(L, 3);
    m->y = (long)luaL_checkinteger(L, 4);
    m->w = (long)luaL_checkinteger(L, 5);
    m->h = (long)luaL_checkinteger(L, 6);
    m->picture = opt_surface(L, 7);
    hold(L, UV_MENUS, k, 7);
    return 0;
}

/*
 * The outline (x, y, w, h, colour), the volume display (surface, x, y,
 * alpha) and the pointer's picture (surface, x, y); each absent when its
 * first argument is nil.
 */
static int l_scene_above(lua_State *L)
{
    struct scene *sc = check_scene(L);

    sc->outline_on = !lua_isnoneornil(L, 2);

    if (sc->outline_on) {
        sc->ox = (long)luaL_checkinteger(L, 2);
        sc->oy = (long)luaL_checkinteger(L, 3);
        sc->ow = (long)luaL_checkinteger(L, 4);
        sc->oh = (long)luaL_checkinteger(L, 5);
        sc->outline_colour = (uint32_t)luaL_checkinteger(L, 6);
    }

    sc->osd = opt_surface(L, 7);
    sc->osd_x = (long)luaL_optinteger(L, 8, 0);
    sc->osd_y = (long)luaL_optinteger(L, 9, 0);
    sc->osd_alpha = (uint32_t)luaL_optinteger(L, 10, 0);
    sc->cursor = opt_surface(L, 11);
    sc->cursor_x = (long)luaL_optinteger(L, 12, 0);
    sc->cursor_y = (long)luaL_optinteger(L, 13, 0);

    hold(L, UV_MISC, MISC_OSD, 7);
    hold(L, UV_MISC, MISC_CURSOR, 11);
    return 0;
}

/* --------------------------------------------------------------------- */
/* Composing.                                                             */
/* --------------------------------------------------------------------- */

/* One damaged rectangle, composed into the back buffer; how many pixels
 * were drawn into it, for `frames`. */
static int l_scene_compose(lua_State *L)
{
    struct scene *sc = check_scene(L);
    long drawn = scene_compose(sc, (long)luaL_checkinteger(L, 2), (long)luaL_checkinteger(L, 3),
                               (long)luaL_checkinteger(L, 4), (long)luaL_checkinteger(L, 5));

    if (drawn < 0) {
        return luaL_error(L, "no memory to compose the scene");
    }

    lua_pushinteger(L, (lua_Integer)drawn);
    return 1;
}

static const luaL_Reg scene_methods[] = {
    { "look",     l_scene_look },
    { "desktop",  l_scene_desktop },
    { "windows",  l_scene_windows },
    { "window",   l_scene_window },
    { "menus",    l_scene_menus },
    { "menu",     l_scene_menu },
    { "above",    l_scene_above },
    { "compose",  l_scene_compose },
    { NULL, NULL }
};

static void scene_open(lua_State *L)
{
    luaL_newmetatable(L, SCENE_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_scene_gc);
    lua_setfield(L, -2, "__gc");
    luaL_setfuncs(L, scene_methods, 0);
    lua_pop(L, 1);
}
