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

#define SCENE_MT "kosmos.scene"

/* What a window is, as bits of `flags`. */
enum {
    SC_HIDDEN   = 1u << 0,
    SC_BLEND    = 1u << 1,      /* the backdrop, or a blended strip */
    SC_ROUNDED  = 1u << 2,      /* an ordinary window: corners and a place
                                   in the occlusion test for them */
    SC_SHADOW   = 1u << 3,
    SC_DIM      = 1u << 4,      /* its shadow lighter: headed, not focused */
    SC_GRIP     = 1u << 5,
    SC_LIGHTS   = 1u << 6,      /* the three, at `bx`, `by` */
    SC_LIT      = 1u << 7,      /* the pointer over them */
    SC_FOCUSED  = 1u << 8,
    SC_MAXIMISE = 1u << 9,      /* it can be maximised */
};

/* The lights, from the left: maximise, minimise, close (`OUT.SLOT`). */
#define SC_SLOTS 3

struct sc_window {
    long x, y, w, h;
    unsigned flags;
    struct surface *picture;
    long src_w, src_h;
    struct surface *bar;        /* a menu bar above the picture, or NULL */
    long bar_h;
    long bx, by;
    int held;                   /* the light pressed, its slot, or -1 */
};

struct sc_menu {
    long x, y, w, h;
    struct surface *picture;
};

struct sc_rect {
    long x, y, w, h;
};

struct scene {
    struct surface *back;

    struct sc_window *win;
    int nwin, capwin;
    struct sc_menu *menu;
    int nmenu, capmenu;

    /* The look. */
    long corner, shadow, grip;
    uint32_t edge_light, edge_dark;
    long box, box_w, run, d;
    uint32_t track, line_soft;
    uint32_t fill[SC_SLOTS], ring[SC_SLOTS], mark[SC_SLOTS], held_fill[SC_SLOTS];
    long outline;

    /* The desktop. */
    uint32_t desktop;
    struct surface *wallpaper, *stamp;
    long wx, wy, ww, wh, sx, sy;

    /* Over everything. */
    bool outline_on;
    long ox, oy, ow, oh;
    uint32_t outline_colour;
    struct surface *osd, *cursor;
    long osd_x, osd_y, cursor_x, cursor_y;
    uint32_t osd_alpha;

    /* Scratch: the pieces of a rectangle not yet covered, two lists that
     * alternate, each `cap_pieces` long, and what each window shows of it -
     * nothing when its width is 0. */
    struct sc_rect *pieces[2];
    int cap_pieces;
    struct sc_rect *visible;
    int cap_visible;

    /* The corners kept from under a window (`OUT.keep_surface`). */
    uint32_t keep_pixels[64 * 64];
    struct surface keep;
};

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

static void *grow(lua_State *L, void *list, int *cap, int want, size_t each)
{
    void *bigger;
    int n;

    if (want <= *cap) {
        return list;
    }

    n = *cap > 0 ? *cap : 8;

    while (n < want) {
        n *= 2;
    }

    bigger = realloc(list, (size_t)n * each);

    if (bigger == NULL) {
        luaL_error(L, "no memory for a scene of %d", want);
    }

    *cap = n;
    return bigger;
}

/* --------------------------------------------------------------------- */
/* Being described.                                                       */
/* --------------------------------------------------------------------- */

static int l_scene(lua_State *L)
{
    struct surface *back = check_surface(L, 1);
    struct scene *sc = lua_newuserdatauv(L, sizeof(*sc), 1);
    int slot;

    memset(sc, 0, sizeof(*sc));
    sc->back = back;
    sc->keep.pixels = sc->keep_pixels;
    sc->keep.width = sc->keep.height = 64;
    sc->keep.pitch = 64 * 4;
    sc->outline = 2;

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

    free(sc->win);
    free(sc->menu);
    free(sc->pieces[0]);
    free(sc->pieces[1]);
    free(sc->visible);
    sc->win = NULL;
    sc->menu = NULL;
    sc->pieces[0] = sc->pieces[1] = NULL;
    sc->visible = NULL;
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
    sc->win = grow(L, sc->win, &sc->capwin, n, sizeof(*sc->win));
    sc->visible = grow(L, sc->visible, &sc->cap_visible, n, sizeof(*sc->visible));
    sc->nwin = n;
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
    sc->menu = grow(L, sc->menu, &sc->capmenu, n, sizeof(*sc->menu));
    sc->nmenu = n;
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

/* `a` with the box (bx0, by0)-(bx1, by1) cut out, appended to `out` as up
 * to four rectangles in bands - above, below, then left and right of what
 * is left - so they never overlap (`subtract_into`). */
static void subtract_into(struct sc_rect *out, int *n, const struct sc_rect *a,
                          long bx0, long by0, long bx1, long by1)
{
    long ax1 = a->x + a->w, ay1 = a->y + a->h;
    long y0, y1;

    if (by0 > a->y) {
        out[(*n)++] = (struct sc_rect){ a->x, a->y, a->w, by0 - a->y };
    }

    if (by1 < ay1) {
        out[(*n)++] = (struct sc_rect){ a->x, by1, a->w, ay1 - by1 };
    }

    y0 = by0 > a->y ? by0 : a->y;
    y1 = by1 < ay1 ? by1 : ay1;

    if (y1 > y0) {
        if (bx0 > a->x) {
            out[(*n)++] = (struct sc_rect){ a->x, y0, bx0 - a->x, y1 - y0 };
        }

        if (bx1 < ax1) {
            out[(*n)++] = (struct sc_rect){ bx1, y0, ax1 - bx1, y1 - y0 };
        }
    }
}

/* What the round corners of a window leave uncovered inside the box it
 * covers: the four corner squares, clipped to it (`OUT.uncover`). */
static void uncover(struct sc_rect *out, int *n, const struct sc_window *w, long c,
                    long x0, long y0, long x1, long y1)
{
    long at[4][2] = {
        { w->x, w->y }, { w->x + w->w - c, w->y },
        { w->x, w->y + w->h - c }, { w->x + w->w - c, w->y + w->h - c },
    };
    int k;

    for (k = 0; k < 4; k++) {
        long ix0 = at[k][0] > x0 ? at[k][0] : x0;
        long iy0 = at[k][1] > y0 ? at[k][1] : y0;
        long ix1 = at[k][0] + c < x1 ? at[k][0] + c : x1;
        long iy1 = at[k][1] + c < y1 ? at[k][1] + c : y1;

        if (ix1 > ix0 && iy1 > iy0) {
            out[(*n)++] = (struct sc_rect){ ix0, iy0, ix1 - ix0, iy1 - iy0 };
        }
    }
}

/* The four corners of a frame inside `r`: where each lies on the back and
 * where it is kept, `x y w h kx ky` (`OUT.corners`). */
static int corners(const struct scene *sc, long fx, long fy, long fw, long fh,
                   const struct sc_rect *r, long out[4][6])
{
    long c = sc->corner;
    long at[4][4] = {
        { fx, fy, 0, 0 }, { fx + fw - c, fy, c, 0 },
        { fx, fy + fh - c, 0, c }, { fx + fw - c, fy + fh - c, c, c },
    };
    int k, n = 0;

    if (c <= 0 || c > 32) {
        return 0;
    }

    for (k = 0; k < 4; k++) {
        long x0 = at[k][0] > r->x ? at[k][0] : r->x;
        long y0 = at[k][1] > r->y ? at[k][1] : r->y;
        long x1 = at[k][0] + c < r->x + r->w ? at[k][0] + c : r->x + r->w;
        long y1 = at[k][1] + c < r->y + r->h ? at[k][1] + c : r->y + r->h;

        if (x1 > x0 && y1 > y0) {
            out[n][0] = x0;
            out[n][1] = y0;
            out[n][2] = x1 - x0;
            out[n][3] = y1 - y0;
            out[n][4] = at[k][2] + (x0 - at[k][0]);
            out[n][5] = at[k][3] + (y0 - at[k][1]);
            n++;
        }
    }

    return n;
}

static void keep_corners(struct scene *sc, long kept[4][6], int n)
{
    int k;

    for (k = 0; k < n; k++) {
        blit_on(&sc->keep, sc->back, kept[k][0], kept[k][1], kept[k][2],
                kept[k][3], kept[k][4], kept[k][5]);
    }
}

static void put_back(struct scene *sc, long kept[4][6], int n,
                     long fx, long fy, long fw, long fh)
{
    int k;

    for (k = 0; k < n; k++) {
        blit_round_on(sc->back, &sc->keep, kept[k][4], kept[k][5], kept[k][2],
                      kept[k][3], kept[k][0], kept[k][1], fx, fy, fw, fh,
                      sc->corner, 1);
    }
}

/* The desktop under what no window covers (`draw_desktop`). */
static void draw_desktop(struct scene *sc, const struct sc_rect *r)
{
    bool covered = sc->wallpaper != NULL
                   && r->x >= sc->wx && r->y >= sc->wy
                   && r->x + r->w <= sc->wx + sc->ww
                   && r->y + r->h <= sc->wy + sc->wh;

    if (!covered) {
        fill_on(sc->back, r->x, r->y, r->w, r->h, sc->desktop);
    }

    if (sc->wallpaper != NULL) {
        blit_on(sc->back, sc->wallpaper, r->x - sc->wx, r->y - sc->wy, r->w, r->h,
                r->x, r->y);
    }

    if (sc->stamp != NULL) {
        long sw = (long)sc->stamp->width, sh = (long)sc->stamp->height;
        long x0 = r->x > sc->sx ? r->x : sc->sx;
        long y0 = r->y > sc->sy ? r->y : sc->sy;
        long x1 = r->x + r->w < sc->sx + sw ? r->x + r->w : sc->sx + sw;
        long y1 = r->y + r->h < sc->sy + sh ? r->y + r->h : sc->sy + sh;

        if (x1 > x0 && y1 > y0) {
            blit_on(sc->back, sc->stamp, x0 - sc->sx, y0 - sc->sy, x1 - x0, y1 - y0,
                    x0, y0);
        }
    }
}

/* One light (`OUT.light`): a disc in its colours, and its glyph when the
 * pointer is over the three. */
static void light(struct scene *sc, long x, long y, int slot, bool lit, bool off,
                  bool held)
{
    long d = sc->d;
    long dx = x + (sc->box - d) / 2, dy = y + (sc->box - d) / 2;
    long cx, cy, k, i;

    if (off) {
        fill_round_on(sc->back, dx, dy, d, d, sc->track, d / 2);
        frame_round_on(sc->back, dx, dy, d, d, sc->line_soft, d / 2);
        return;
    }

    fill_round_on(sc->back, dx, dy, d, d, held ? sc->held_fill[slot] : sc->fill[slot],
                  d / 2);
    frame_round_on(sc->back, dx, dy, d, d, sc->ring[slot], d / 2);

    if (!lit) {
        return;
    }

    cx = dx + d / 2;
    cy = dy + d / 2;
    k = d / 4;

    if (slot == 1) {                                    /* minimise */
        fill_on(sc->back, cx - k - 1, cy - 1, 2 * k + 2, 2, sc->mark[slot]);
    } else if (slot == 2) {                             /* close */
        for (i = -k; i <= k; i++) {
            fill_on(sc->back, cx + i - 1, cy + i - 1, 2, 2, sc->mark[slot]);
            fill_on(sc->back, cx - i - 1, cy + i - 1, 2, 2, sc->mark[slot]);
        }
    } else {                                            /* maximise */
        double ax[3] = { (double)(cx - k - 1), (double)(cx + k), (double)(cx + k) };
        double ay[3] = { (double)(cy - k), (double)(cy - k), (double)(cy + k - 1) };
        double bx[3] = { (double)(cx - k), (double)(cx - k), (double)(cx + k - 1) };
        double by[3] = { (double)(cy - k + 1), (double)(cy + k), (double)(cy + k) };

        triangle_on(sc->back, ax, ay, sc->mark[slot]);
        triangle_on(sc->back, bx, by, sc->mark[slot]);
    }
}

/* A window's part of `r` (`drawwindow.lua`). */
static void draw_window(struct scene *sc, const struct sc_window *w, const struct sc_rect *r)
{
    long kept[4][6];
    int nkept = 0;
    long x0, y0, x1, y1;

    if ((w->flags & SC_HIDDEN) || w->x >= r->x + r->w || w->x + w->w <= r->x
        || w->y >= r->y + r->h || w->y + w->h <= r->y) {
        return;
    }

    if (w->flags & SC_ROUNDED) {
        nkept = corners(sc, w->x, w->y, w->w, w->h, r, kept);
        keep_corners(sc, kept, nkept);
    }

    x0 = w->x > r->x ? w->x : r->x;
    y0 = w->y > r->y ? w->y : r->y;
    x1 = w->x + w->w < r->x + r->w ? w->x + w->w : r->x + r->w;
    y1 = w->y + w->h < r->y + r->h ? w->y + w->h : r->y + r->h;

    if (x1 > x0 && y1 > y0 && w->picture != NULL) {
        if (w->flags & SC_BLEND) {
            blend_on(sc->back, w->picture, x0 - w->x, y0 - w->y, x1 - x0, y1 - y0,
                     x0, y0, 255);
        } else if (w->bar != NULL) {
            /* A menu bar above the picture (`strips.compose`). */
            long split = w->y + w->bar_h;

            if (y0 < split) {
                long yb = y1 < split ? y1 : split;

                blit_on(sc->back, w->bar, x0 - w->x, y0 - w->y, x1 - x0, yb - y0, x0, y0);
            }

            if (y1 > split) {
                long ya = y0 > split ? y0 : split;
                long lower = w->h - w->bar_h;

                if (w->src_w != w->w || w->src_h != lower) {
                    stretch_into(sc->back, w->picture, 0, 0, w->src_w, w->src_h,
                                 w->x, split, w->w, lower, -1, false,
                                 x0, ya, x1, y1);
                } else {
                    blit_on(sc->back, w->picture, x0 - w->x, ya - split, x1 - x0, y1 - ya,
                            x0, ya);
                }
            }
        } else if (w->src_w != w->w || w->src_h != w->h) {
            stretch_into(sc->back, w->picture, 0, 0, w->src_w, w->src_h,
                         w->x, w->y, w->w, w->h, -1, false, x0, y0, x1, y1);
        } else {
            blit_on(sc->back, w->picture, x0 - w->x, y0 - w->y, x1 - x0, y1 - y0, x0, y0);
        }
    }

    /* The sizing grip, three diagonal steps over its bottom-right corner. */
    if (w->flags & SC_GRIP) {
        long gx = w->x + w->w - sc->grip, gy = w->y + w->h - sc->grip;
        long step;

        for (step = 0; step <= 2; step++) {
            long o = step * 5, n = sc->grip - 3 - o;

            if (n > 0) {
                fill_on(sc->back, gx + o + 2, gy + sc->grip - 3 - o, n, 1, sc->edge_light);
                fill_on(sc->back, gx + o + 2, gy + sc->grip - 2 - o, n, 1, sc->edge_dark);
            }
        }
    }

    /* The three, over its header. */
    if ((w->flags & SC_LIGHTS) && w->bx < r->x + r->w && w->bx + sc->run > r->x
        && w->by < r->y + r->h && w->by + sc->box > r->y) {
        int slot;

        for (slot = 0; slot < SC_SLOTS; slot++) {
            bool held = w->held == slot;
            bool off = !(w->flags & SC_FOCUSED) || (slot == 0 && !(w->flags & SC_MAXIMISE));

            light(sc, w->bx + sc->box_w * slot, w->by, slot,
                  (w->flags & SC_LIT) || held, off, held);
        }
    }

    if (nkept > 0) {
        put_back(sc, kept, nkept, w->x, w->y, w->w, w->h);
    }
}

/* A window's shadow, under it (`OUT.cast_shadow`). */
static void cast_shadow(struct scene *sc, const struct sc_window *w, const struct sc_rect *r)
{
    long s = sc->shadow;
    long sx, sy, sw, sh;
    long clip[4];

    if (s <= 0 || !(w->flags & SC_SHADOW) || (w->flags & SC_HIDDEN)) {
        return;
    }

    sx = w->x - s;
    sy = w->y - s + s / 3;
    sw = w->w + s * 2;
    sh = w->h + s * 2;

    if (sx >= r->x + r->w || sx + sw <= r->x || sy >= r->y + r->h || sy + sh <= r->y) {
        return;
    }

    clip[0] = r->x;
    clip[1] = r->y;
    clip[2] = r->w;
    clip[3] = r->h;
    gfx_shadow(sc->back->pixels, sc->back->pitch, (long)sc->back->width,
               (long)sc->back->height, w->x, w->y, w->w, w->h, sc->corner, s,
               (w->flags & SC_DIM) ? 55 : 90, clip);
}

/* Both lists of pieces at least `want` long; they keep what they hold. */
static void pieces_room(lua_State *L, struct scene *sc, int want)
{
    int cap;

    if (want <= sc->cap_pieces && sc->pieces[0] != NULL) {
        return;
    }

    cap = sc->cap_pieces;
    sc->pieces[0] = grow(L, sc->pieces[0], &cap, want < 64 ? 64 : want,
                         sizeof(struct sc_rect));
    cap = sc->cap_pieces;
    sc->pieces[1] = grow(L, sc->pieces[1], &cap, want < 64 ? 64 : want,
                         sizeof(struct sc_rect));
    sc->cap_pieces = cap;
}

/* One damaged rectangle, composed into the back buffer; how many pixels
 * were drawn into it, for `frames`. */
static int l_scene_compose(lua_State *L)
{
    struct scene *sc = check_scene(L);
    struct sc_rect r = { (long)luaL_checkinteger(L, 2), (long)luaL_checkinteger(L, 3),
                         (long)luaL_checkinteger(L, 4), (long)luaL_checkinteger(L, 5) };
    int cur = 0, nrem = 1, nkeep, k, p;
    struct sc_rect *remaining, *keep;
    lua_Integer drawn = 0;

    if (sc->back->pixels == NULL || r.w <= 0 || r.h <= 0) {
        lua_pushinteger(L, 0);
        return 1;
    }

    pieces_room(L, sc, 1);
    sc->pieces[cur][0] = r;

    /* Front to back: what each window shows of what is left uncovered. */
    for (k = sc->nwin - 1; k >= 0; k--) {
        struct sc_window *w = &sc->win[k];
        struct sc_rect *v = &sc->visible[k];
        long c = sc->corner;
        bool round = (w->flags & SC_ROUNDED) && c > 0 && c <= 32;
        bool mine = false;

        v->w = 0;

        if (nrem == 0 || (w->flags & SC_HIDDEN)) {
            continue;
        }

        /* A piece becomes at most eight: four around the window, and the
         * four corner squares its rounding leaves showing. */
        pieces_room(L, sc, nrem * 8);
        remaining = sc->pieces[cur];
        keep = sc->pieces[1 - cur];
        nkeep = 0;

        for (p = 0; p < nrem; p++) {
            struct sc_rect *piece = &remaining[p];
            long x0 = w->x > piece->x ? w->x : piece->x;
            long y0 = w->y > piece->y ? w->y : piece->y;
            long x1 = w->x + w->w < piece->x + piece->w ? w->x + w->w : piece->x + piece->w;
            long y1 = w->y + w->h < piece->y + piece->h ? w->y + w->h : piece->y + piece->h;

            if (x1 > x0 && y1 > y0) {
                if (mine) {
                    long vx1 = v->x + v->w, vy1 = v->y + v->h;

                    if (x0 < v->x) v->x = x0;
                    if (y0 < v->y) v->y = y0;
                    if (x1 > vx1) vx1 = x1;
                    if (y1 > vy1) vy1 = y1;
                    v->w = vx1 - v->x;
                    v->h = vy1 - v->y;
                } else {
                    *v = (struct sc_rect){ x0, y0, x1 - x0, y1 - y0 };
                    mine = true;
                }

                if (w->flags & SC_BLEND) {
                    keep[nkeep++] = *piece;
                } else {
                    subtract_into(keep, &nkeep, piece, x0, y0, x1, y1);

                    if (round) {
                        uncover(keep, &nkeep, w, c, x0, y0, x1, y1);
                    }
                }
            } else {
                keep[nkeep++] = *piece;
            }
        }

        cur = 1 - cur;
        nrem = nkeep;
    }

    remaining = sc->pieces[cur];

    for (p = 0; p < nrem; p++) {
        drawn += (lua_Integer)remaining[p].w * remaining[p].h;
        draw_desktop(sc, &remaining[p]);
    }

    /* Back to front: each window's shadow, then what it shows. */
    for (k = 0; k < sc->nwin; k++) {
        cast_shadow(sc, &sc->win[k], &r);

        if (sc->visible[k].w > 0) {
            drawn += (lua_Integer)sc->visible[k].w * sc->visible[k].h;
            draw_window(sc, &sc->win[k], &sc->visible[k]);
        }
    }

    /* Menus, above every window, rounded as a window is. */
    for (k = 0; k < sc->nmenu; k++) {
        struct sc_menu *m = &sc->menu[k];
        long x0 = m->x > r.x ? m->x : r.x;
        long y0 = m->y > r.y ? m->y : r.y;
        long x1 = m->x + m->w < r.x + r.w ? m->x + m->w : r.x + r.w;
        long y1 = m->y + m->h < r.y + r.h ? m->y + m->h : r.y + r.h;
        long kept[4][6];
        int nkept;

        if (x1 <= x0 || y1 <= y0 || m->picture == NULL) {
            continue;
        }

        nkept = corners(sc, m->x, m->y, m->w, m->h, &r, kept);
        keep_corners(sc, kept, nkept);
        blit_on(sc->back, m->picture, x0 - m->x, y0 - m->y, x1 - x0, y1 - y0, x0, y0);
        put_back(sc, kept, nkept, m->x, m->y, m->w, m->h);
    }

    /* The rubber band. */
    if (sc->outline_on) {
        long o = sc->outline;

        fill_on(sc->back, sc->ox, sc->oy, sc->ow, o, sc->outline_colour);
        fill_on(sc->back, sc->ox, sc->oy + sc->oh - o, sc->ow, o, sc->outline_colour);
        fill_on(sc->back, sc->ox, sc->oy, o, sc->oh, sc->outline_colour);
        fill_on(sc->back, sc->ox + sc->ow - o, sc->oy, o, sc->oh, sc->outline_colour);
    }

    /* The level bar. */
    if (sc->osd != NULL && sc->osd_alpha > 0) {
        long ow = (long)sc->osd->width, oh = (long)sc->osd->height;
        long x0 = r.x > sc->osd_x ? r.x : sc->osd_x;
        long y0 = r.y > sc->osd_y ? r.y : sc->osd_y;
        long x1 = r.x + r.w < sc->osd_x + ow ? r.x + r.w : sc->osd_x + ow;
        long y1 = r.y + r.h < sc->osd_y + oh ? r.y + r.h : sc->osd_y + oh;

        if (x1 > x0 && y1 > y0) {
            blend_on(sc->back, sc->osd, x0 - sc->osd_x, y0 - sc->osd_y, x1 - x0, y1 - y0,
                     x0, y0, sc->osd_alpha);
        }
    }

    /* The pointer, where the display does not draw it. */
    if (sc->cursor != NULL) {
        blend_on(sc->back, sc->cursor, 0, 0, (long)sc->cursor->width,
                 (long)sc->cursor->height, sc->cursor_x, sc->cursor_y, 255);
    }

    lua_pushinteger(L, drawn);
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
