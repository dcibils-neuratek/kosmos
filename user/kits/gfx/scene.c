/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The scene: the desktop composed in C (`docs/astra-display.md`, D3a).
 *
 *   local sc = gfx.scene(back)
 *   sc:look(corner, shadow, grip, edge_light, edge_dark, box, box_w, run, d,
 *           track, line_soft, outline, lights...)       what it looks like
 *   sc:desktop(colour, wallpaper, wx, wy, ww, wh, stamp, sx, sy, glass)
 *   sc:rim(bevel, refraction, specular, front, split)   a glass edge's lens,
 *                                                       its light in front,
 *                                                       its colours apart
 *   sc:windows(n)                                       how many, in order
 *   sc:window(k, x, y, w, h, flags, picture, src_w, src_h,
 *             bar, bar_h, bx, by, held)                 the k-th from the back
 *   sc:inset(k, picture, ix, iy, iw, ih)              a second picture over it
 *   sc:plate(k, picture, px, py)                      Starlight's plate over it
 *   sc:menus(n)  sc:menu(k, x, y, w, h, picture[, flags])  above every window;
 *                                                       flags 4096 is glass
 *   sc:above(outline x, y, w, h, colour | nil, osd, ox, oy, alpha | nil,
 *            cursor, cx, cy | nil, badge, bx, by | nil,
 *            panel, px, py | nil)                       over everything
 *   sc:compose(x, y, w, h)  -> pixels drawn             one damaged rectangle
 *   sc:wire(at, bytes, damage) -> true | false, need    for the window server
 *
 * **Described for the window server** (D3b-2), a picture is the slot it was
 * handed over in - a number - rather than a surface, and `wire` writes the
 * scene into the region the two share, which the server composes from.
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
#include "blur.h"
#include "windowproto.h"

#define SCENE_MT "kosmos.scene"

/* The user value's slots: what the scene names, kept alive. */
enum { UV_WINDOWS = 1, UV_BARS, UV_MENUS, UV_INSETS, UV_PLATES, UV_MISC, UV_COUNT = UV_MISC };
enum { MISC_WALLPAPER = 1, MISC_STAMP, MISC_OSD, MISC_CURSOR, MISC_BADGE, MISC_PANEL,
       MISC_GLASS };

static struct scene *check_scene(lua_State *L)
{
    return luaL_checkudata(L, 1, SCENE_MT);
}

/*
 * A picture argument: a surface, nil, or - described for the window server
 * (D3b-2) - the slot it was handed over in, a number. A freed surface is as
 * good as none; `*slot` is the number, or `WS_NONE`.
 */
static struct surface *opt_picture(lua_State *L, int index, int *slot)
{
    struct surface *s;

    *slot = WS_NONE;

    if (lua_isnoneornil(L, index)) {
        return NULL;
    }

    if (lua_type(L, index) == LUA_TNUMBER) {
        lua_Integer n = luaL_checkinteger(L, index);

        luaL_argcheck(L, n >= 0 && n < (lua_Integer)WS_SLOTS_MAX, index, "no such slot");
        *slot = (int)n;
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
    sc->wallpaper_slot = sc->stamp_slot = sc->osd_slot = WS_NONE;
    sc->glass_slot = WS_NONE;
    sc->cursor_slot = sc->badge_slot = sc->panel_slot = WS_NONE;

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

    /* The cut on a menu's top-left, a look's (Starlight's); 0 for none. */
    sc->cut = (long)luaL_optinteger(L, at, 0);
    if (sc->cut < 0 || sc->cut > 32) sc->cut = 0;

    return 0;
}

static int l_scene_desktop(lua_State *L)
{
    struct scene *sc = check_scene(L);

    sc->desktop   = (uint32_t)luaL_checkinteger(L, 2);
    sc->wallpaper = opt_picture(L, 3, &sc->wallpaper_slot);
    sc->wx = (long)luaL_optinteger(L, 4, 0);
    sc->wy = (long)luaL_optinteger(L, 5, 0);
    sc->ww = (long)luaL_optinteger(L, 6, 0);
    sc->wh = (long)luaL_optinteger(L, 7, 0);
    sc->stamp = opt_picture(L, 8, &sc->stamp_slot);
    sc->sx = (long)luaL_optinteger(L, 9, 0);
    sc->sy = (long)luaL_optinteger(L, 10, 0);
    /* The wallpaper's glass, lying where the wallpaper lies (LG4). */
    sc->glass = opt_picture(L, 11, &sc->glass_slot);

    hold(L, UV_MISC, MISC_WALLPAPER, 3);
    hold(L, UV_MISC, MISC_STAMP, 8);
    hold(L, UV_MISC, MISC_GLASS, 11);
    return 0;
}

/*
 * The rim of a glass window and menu (Liquid Glass LG3): its bevel in
 * pixels and its refraction and specular in percent, the look's; a bevel
 * of 0 for none. Out of range is none, rather than an error: the lens is
 * a look's, and a look with no rim is a look.
 */
static int l_scene_rim(lua_State *L)
{
    struct scene *sc = check_scene(L);
    long b = (long)luaL_optinteger(L, 2, 0);
    long r = (long)luaL_optinteger(L, 3, 0);
    long s = (long)luaL_optinteger(L, 4, 0);

    if (b < 0 || b > (long)GFX_LENS_MOST || r < 0 || r > 100 || s < 0 || s > 100) {
        b = r = s = 0;
    }

    sc->rim_bevel = b;
    sc->rim_refraction = r;
    sc->rim_specular = s;
    sc->rim_front = (uint32_t)luaL_optinteger(L, 5, 0);
    sc->rim_split = (long)luaL_optinteger(L, 6, 0);
    if (sc->rim_split < 0 || sc->rim_split > 100) sc->rim_split = 0;
    return 0;
}

/*
 * **Live glass** (Liquid Glass LG6): the floating surfaces over what is
 * under them, blurred at radius `r` in three passes - 0 for clear glass -
 * or nil for none, wallpaper glass. Past what the blur takes is none.
 */
static int l_scene_live(lua_State *L)
{
    struct scene *sc = check_scene(L);
    long r = lua_isnoneornil(L, 2) ? -1 : (long)luaL_checkinteger(L, 2);

    sc->live_blur = r >= 0 && r <= (long)GFX_BLUR_MOST ? r : -1;
    return 0;
}

/*
 * The damage, a list of `{x, y, w, h}`, widened in place for live glass
 * (`scene_widen`): a rectangle touching a live surface's reach grows to
 * hold it. How many grew.
 */
static int l_scene_widen(lua_State *L)
{
    struct scene *sc = check_scene(L);
    int n, i, grew;
    struct sc_rect *r;

    luaL_checktype(L, 2, LUA_TTABLE);
    n = (int)lua_rawlen(L, 2);
    if (n == 0 || sc->live_blur < 0) {
        lua_pushinteger(L, 0);
        return 1;
    }

    r = malloc((size_t)n * sizeof(*r));
    if (r == NULL) return luaL_error(L, "no memory to widen %d rectangles", n);

    for (i = 0; i < n; i++) {
        lua_rawgeti(L, 2, i + 1);
        lua_getfield(L, -1, "x"); r[i].x = (long)lua_tointeger(L, -1);
        lua_getfield(L, -2, "y"); r[i].y = (long)lua_tointeger(L, -1);
        lua_getfield(L, -3, "w"); r[i].w = (long)lua_tointeger(L, -1);
        lua_getfield(L, -4, "h"); r[i].h = (long)lua_tointeger(L, -1);
        lua_pop(L, 5);
    }

    grew = scene_widen(sc, r, n);

    for (i = 0; grew > 0 && i < n; i++) {
        lua_rawgeti(L, 2, i + 1);
        lua_pushinteger(L, r[i].x); lua_setfield(L, -2, "x");
        lua_pushinteger(L, r[i].y); lua_setfield(L, -2, "y");
        lua_pushinteger(L, r[i].w); lua_setfield(L, -2, "w");
        lua_pushinteger(L, r[i].h); lua_setfield(L, -2, "h");
        lua_pop(L, 1);
    }

    free(r);
    lua_pushinteger(L, grew);
    return 1;
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
    let_go(L, UV_INSETS, n, had);
    let_go(L, UV_PLATES, n, had);
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
    w->picture = opt_picture(L, 8, &w->picture_slot);
    w->src_w   = (long)luaL_optinteger(L, 9, w->w);
    w->src_h   = (long)luaL_optinteger(L, 10, w->h);
    w->bar     = opt_picture(L, 11, &w->bar_slot);
    w->bar_h   = (long)luaL_optinteger(L, 12, 0);
    w->bx      = (long)luaL_optinteger(L, 13, 0);
    w->by      = (long)luaL_optinteger(L, 14, 0);
    w->held    = (int)luaL_optinteger(L, 15, -1);
    w->bx2     = (long)luaL_optinteger(L, 16, 0);

    /* No inset unless `sc:inset` says so, after this, on the same pass;
     * nor a plate unless `sc:plate` does. */
    w->inset = NULL;
    w->inset_slot = WS_NONE;
    w->ix = w->iy = w->iw = w->ih = 0;
    w->plate = NULL;
    w->plate_slot = WS_NONE;
    w->px = w->py = 0;
    w->rx = w->ry = w->rw = w->rh = w->rr = 0;

    hold(L, UV_WINDOWS, k, 8);
    hold(L, UV_BARS, k, 11);
    lua_pushnil(L);
    hold(L, UV_INSETS, k, -1);
    hold(L, UV_PLATES, k, -1);
    lua_pop(L, 1);
    return 0;
}

/*
 * `sc:plate(k, picture, px, py)`: the k-th window's plate - Starlight's
 * title on its amber trapezoid - blended over it at `px`, `py` from its
 * corner (`sc_window`). Said after `sc:window(k, ...)`, which clears it.
 */
static int l_scene_plate(lua_State *L)
{
    struct scene *sc = check_scene(L);
    int k = (int)luaL_checkinteger(L, 2);
    struct sc_window *w;

    luaL_argcheck(L, k >= 1 && k <= sc->nwin, 2, "no such window in the scene");
    w = &sc->win[k - 1];
    w->plate = opt_picture(L, 3, &w->plate_slot);
    w->px = (long)luaL_checkinteger(L, 4);
    w->py = (long)luaL_checkinteger(L, 5);

    hold(L, UV_PLATES, k, 3);
    return 0;
}

/*
 * `sc:shape(k, rx, ry, rw, rh, radius)`: the k-th window's glass shape - a
 * glass strip's rounded rectangle inside its picture, from its corner, in
 * screen pixels (`sc_window.rr`). Said after `sc:window(k, ...)`, which
 * clears it, on every pass it shows.
 */
static int l_scene_shape(lua_State *L)
{
    struct scene *sc = check_scene(L);
    int k = (int)luaL_checkinteger(L, 2);
    struct sc_window *w;

    luaL_argcheck(L, k >= 1 && k <= sc->nwin, 2, "no such window in the scene");
    w = &sc->win[k - 1];
    w->rx = (long)luaL_checkinteger(L, 3);
    w->ry = (long)luaL_checkinteger(L, 4);
    w->rw = (long)luaL_checkinteger(L, 5);
    w->rh = (long)luaL_checkinteger(L, 6);
    w->rr = (long)luaL_checkinteger(L, 7);
    return 0;
}

/*
 * `sc:inset(k, picture, ix, iy, iw, ih)`: the k-th window's second picture,
 * laid over its first at `ix`, `iy` in the first's pixels (`sc_window`).
 * Said after `sc:window(k, ...)`, which clears it, on every pass it shows.
 */
static int l_scene_inset(lua_State *L)
{
    struct scene *sc = check_scene(L);
    int k = (int)luaL_checkinteger(L, 2);
    struct sc_window *w;

    luaL_argcheck(L, k >= 1 && k <= sc->nwin, 2, "no such window in the scene");
    w = &sc->win[k - 1];
    w->inset = opt_picture(L, 3, &w->inset_slot);
    w->ix = (long)luaL_checkinteger(L, 4);
    w->iy = (long)luaL_checkinteger(L, 5);
    w->iw = (long)luaL_checkinteger(L, 6);
    w->ih = (long)luaL_checkinteger(L, 7);

    if (w->iw < 0) {
        w->iw = 0;
    }
    if (w->ih < 0) {
        w->ih = 0;
    }
    if (w->inset != NULL) {
        if ((unsigned long)w->iw > w->inset->width) {
            w->iw = (long)w->inset->width;
        }
        if ((unsigned long)w->ih > w->inset->height) {
            w->ih = (long)w->inset->height;
        }
    }

    hold(L, UV_INSETS, k, 3);
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
    m->picture = opt_picture(L, 7, &m->picture_slot);
    m->flags = (unsigned)luaL_optinteger(L, 8, 0) & (SC_GLASS | SC_LIVE);
    hold(L, UV_MENUS, k, 7);
    return 0;
}

/*
 * The outline (x, y, w, h, colour), the volume display (surface, x, y,
 * alpha), the pointer's picture (surface, x, y) and a drag's label
 * (surface, x, y); each absent when its first argument is nil.
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

    sc->osd = opt_picture(L, 7, &sc->osd_slot);
    sc->osd_x = (long)luaL_optinteger(L, 8, 0);
    sc->osd_y = (long)luaL_optinteger(L, 9, 0);
    sc->osd_alpha = (uint32_t)luaL_optinteger(L, 10, 0);
    sc->cursor = opt_picture(L, 11, &sc->cursor_slot);
    sc->cursor_x = (long)luaL_optinteger(L, 12, 0);
    sc->cursor_y = (long)luaL_optinteger(L, 13, 0);
    sc->badge = opt_picture(L, 14, &sc->badge_slot);
    sc->badge_x = (long)luaL_optinteger(L, 15, 0);
    sc->badge_y = (long)luaL_optinteger(L, 16, 0);
    sc->panel = opt_picture(L, 17, &sc->panel_slot);
    sc->panel_x = (long)luaL_optinteger(L, 18, 0);
    sc->panel_y = (long)luaL_optinteger(L, 19, 0);

    hold(L, UV_MISC, MISC_OSD, 7);
    hold(L, UV_MISC, MISC_CURSOR, 11);
    hold(L, UV_MISC, MISC_BADGE, 14);
    hold(L, UV_MISC, MISC_PANEL, 17);
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

/* --------------------------------------------------------------------- */
/* Written for the window server.                                         */
/* --------------------------------------------------------------------- */

/*
 * `sc:wire(at, bytes, damage)` - the scene as described, written into the
 * region the window server reads it from (`windowproto.h`, D3b-2), mapped
 * here at `at` and `bytes` long, with `damage`'s rectangles - a list of
 * `{ x, y, w, h }`. Its pictures go by slot, so it is described with the
 * slots they were handed over in rather than the surfaces. True; or false
 * and the bytes it needs, when the region is too small for it.
 */
static int l_scene_wire(lua_State *L)
{
    struct scene *sc = check_scene(L);
    lua_Integer at = luaL_checkinteger(L, 2);
    lua_Integer bytes = luaL_checkinteger(L, 3);
    lua_Integer nrect;
    size_t need;
    struct ws_scene_head *head;
    struct ws_scene_window *win;
    struct ws_scene_menu *menu;
    struct ws_rect *rect;
    int k;

    luaL_checktype(L, 4, LUA_TTABLE);
    luaL_argcheck(L, at != 0 && bytes > 0, 2, "the scene's region, mapped");
    nrect = (lua_Integer)lua_rawlen(L, 4);

    need = sizeof(*head) + (size_t)sc->nwin * sizeof(*win)
           + (size_t)sc->nmenu * sizeof(*menu) + (size_t)nrect * sizeof(*rect);

    if (need > (size_t)bytes) {
        lua_pushboolean(L, 0);
        lua_pushinteger(L, (lua_Integer)need);
        return 2;
    }

    head = (struct ws_scene_head *)(uintptr_t)at;
    win = (struct ws_scene_window *)(head + 1);
    menu = (struct ws_scene_menu *)(win + sc->nwin);
    rect = (struct ws_rect *)(menu + sc->nmenu);

    memset(head, 0, sizeof(*head));
    head->magic = WS_SCENE_MAGIC;
    head->nwin = (uint32_t)sc->nwin;
    head->nmenu = (uint32_t)sc->nmenu;
    head->nrect = (uint32_t)nrect;

    head->corner = (int32_t)sc->corner;
    head->shadow = (int32_t)sc->shadow;
    head->grip = (int32_t)sc->grip;
    head->edge_light = sc->edge_light;
    head->edge_dark = sc->edge_dark;
    head->box = (int32_t)sc->box;
    head->box_w = (int32_t)sc->box_w;
    head->run = (int32_t)sc->run;
    head->d = (int32_t)sc->d;
    head->track = sc->track;
    head->line_soft = sc->line_soft;
    head->outline = (int32_t)sc->outline;
    head->cut = (int32_t)sc->cut;

    for (k = 0; k < SC_SLOTS; k++) {
        head->fill[k] = sc->fill[k];
        head->ring[k] = sc->ring[k];
        head->mark[k] = sc->mark[k];
        head->held_fill[k] = sc->held_fill[k];
    }

    head->desktop = sc->desktop;
    head->wallpaper = (int32_t)sc->wallpaper_slot;
    head->wx = (int32_t)sc->wx;
    head->wy = (int32_t)sc->wy;
    head->ww = (int32_t)sc->ww;
    head->wh = (int32_t)sc->wh;
    head->stamp = (int32_t)sc->stamp_slot;
    head->sx = (int32_t)sc->sx;
    head->sy = (int32_t)sc->sy;
    head->glass = (int32_t)sc->glass_slot;
    head->rim_bevel = (int32_t)sc->rim_bevel;
    head->rim_refraction = (int32_t)sc->rim_refraction;
    head->rim_specular = (int32_t)sc->rim_specular;
    head->rim_front = sc->rim_front;
    head->rim_split = (int32_t)sc->rim_split;
    head->live_blur = (int32_t)sc->live_blur;

    head->outline_on = sc->outline_on ? 1u : 0u;
    head->ox = (int32_t)sc->ox;
    head->oy = (int32_t)sc->oy;
    head->ow = (int32_t)sc->ow;
    head->oh = (int32_t)sc->oh;
    head->outline_colour = sc->outline_colour;
    head->osd = (int32_t)sc->osd_slot;
    head->osd_x = (int32_t)sc->osd_x;
    head->osd_y = (int32_t)sc->osd_y;
    head->osd_alpha = sc->osd_alpha;
    head->cursor = (int32_t)sc->cursor_slot;
    head->cursor_x = (int32_t)sc->cursor_x;
    head->cursor_y = (int32_t)sc->cursor_y;
    head->badge = (int32_t)sc->badge_slot;
    head->badge_x = (int32_t)sc->badge_x;
    head->badge_y = (int32_t)sc->badge_y;
    head->panel = (int32_t)sc->panel_slot;
    head->panel_x = (int32_t)sc->panel_x;
    head->panel_y = (int32_t)sc->panel_y;

    for (k = 0; k < sc->nwin; k++) {
        const struct sc_window *w = &sc->win[k];

        win[k] = (struct ws_scene_window){
            (int32_t)w->x, (int32_t)w->y, (int32_t)w->w, (int32_t)w->h, w->flags,
            (int32_t)w->picture_slot, (int32_t)w->src_w, (int32_t)w->src_h,
            (int32_t)w->bar_slot, (int32_t)w->bar_h,
            (int32_t)w->bx, (int32_t)w->by, (int32_t)w->held, (int32_t)w->bx2,
            (int32_t)w->inset_slot, (int32_t)w->ix, (int32_t)w->iy,
            (int32_t)w->iw, (int32_t)w->ih,
            (int32_t)w->plate_slot, (int32_t)w->px, (int32_t)w->py,
            (int32_t)w->rx, (int32_t)w->ry, (int32_t)w->rw, (int32_t)w->rh,
            (int32_t)w->rr
        };
    }

    for (k = 0; k < sc->nmenu; k++) {
        const struct sc_menu *m = &sc->menu[k];

        menu[k] = (struct ws_scene_menu){
            (int32_t)m->x, (int32_t)m->y, (int32_t)m->w, (int32_t)m->h,
            (int32_t)m->picture_slot, (uint32_t)m->flags
        };
    }

    for (k = 0; k < (int)nrect; k++) {
        lua_rawgeti(L, 4, k + 1);
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_getfield(L, -1, "x");
        lua_getfield(L, -2, "y");
        lua_getfield(L, -3, "w");
        lua_getfield(L, -4, "h");
        rect[k] = (struct ws_rect){
            (int32_t)luaL_checkinteger(L, -4), (int32_t)luaL_checkinteger(L, -3),
            (int32_t)luaL_checkinteger(L, -2), (int32_t)luaL_checkinteger(L, -1)
        };
        lua_pop(L, 5);
    }

    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg scene_methods[] = {
    { "look",     l_scene_look },
    { "desktop",  l_scene_desktop },
    { "rim",      l_scene_rim },
    { "live",     l_scene_live },
    { "widen",    l_scene_widen },
    { "windows",  l_scene_windows },
    { "window",   l_scene_window },
    { "inset",    l_scene_inset },
    { "plate",    l_scene_plate },
    { "shape",    l_scene_shape },
    { "menus",    l_scene_menus },
    { "menu",     l_scene_menu },
    { "above",    l_scene_above },
    { "compose",  l_scene_compose },
    { "wire",     l_scene_wire },
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
