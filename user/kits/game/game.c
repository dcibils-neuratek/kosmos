/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Game Kit's drawing layer, in C.
 *
 *   local game = use("/kits/game")
 *   game.clear(fb, w, h, 0x000000)
 *   game.line(fb, w, h, x0, y0, x1, y1, r, g, b, a)
 *
 * **What this is for**, in Diego's words on 21 September: "this planet sim
 * is a great exercise for us to create a reusable kit of high performance
 * components for games and graphical apps that run at full speed in c",
 * "while lua orchestrates them". That is `CLAUDE.md`'s language split said
 * from the application's side, and it is the whole design: Lua decides
 * *what* is drawn and *where*, and every loop that runs once per pixel is
 * here.
 *
 * `roadmap.md` 4f is the kit this belongs to. These two primitives are its
 * first, chosen by measurement rather than by taste: in the solar system's
 * overview the orbits are 31% of a frame and clearing the framebuffer is
 * another 11%, so an anti-aliased line and a fill are two fifths of the
 * work before anything else is touched.
 *
 * **Two kinds of target, one implementation.** A Kosmos surface is what a
 * native application draws into - flat bytes, a pitch, and the compositor
 * on the other side of it. A Lua array of `0xRRGGBB` numbers is what a
 * *portable* renderer produces, one written to run on any interpreter
 * (`user/lib/solar`), and it cannot be asked to change its representation
 * without ceasing to be portable. So both are accepted, the fast one is
 * fast and the portable one is still far quicker than the interpreter, and
 * a program that later moves from a table to a surface changes nothing but
 * the value it passes in.
 */

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#include "lua.h"
#include "lauxlib.h"

/*
 * The one door into a surface from outside `gfx.c`, which exists already:
 * it was exported for Doom, which renders into a buffer of its own and
 * needs the result copied in. A second way in would be a second thing to
 * keep in step with the struct.
 */
uint32_t *kosmos_surface_pixels(lua_State *L, int index,
                                unsigned *w, unsigned *h, unsigned *pitch);

/*
 * Where the pixels are, whichever kind they are.
 *
 * `table` is the stack index of a Lua array when that is the target, and
 * zero when the target is a surface. One struct, so the drawing below is
 * written once and asks a question per pixel that the branch predictor
 * answers for free.
 */
struct target {
    lua_State *L;
    int        table;       /* the stack index of an array, or 0 */
    uint32_t  *pixels;      /* a surface's bytes, or NULL */
    unsigned   pitch;       /* bytes to the next row; never width * 4 */
    int        w;
    int        h;
};

static struct target target_of(lua_State *L, int index, int w_index)
{
    struct target t;

    t.L = L;
    t.table = 0;
    t.pixels = NULL;
    t.pitch = 0;

    if (lua_type(L, index) == LUA_TTABLE) {
        t.table = index;
        t.w = (int)luaL_checkinteger(L, w_index);
        t.h = (int)luaL_checkinteger(L, w_index + 1);
    } else {
        unsigned w = 0, h = 0, pitch = 0;

        t.pixels = kosmos_surface_pixels(L, index, &w, &h, &pitch);
        t.pitch = pitch;
        t.w = (int)w;
        t.h = (int)h;

        /*
         * The size is read from the surface, and the two arguments that
         * would have said it are accepted and ignored - so a caller that
         * moves from a table to a surface does not have to rewrite its
         * call sites, which is the point of taking both.
         */
    }

    return t;
}

static uint32_t *row_of(const struct target *t, int y)
{
    return (uint32_t *)((uint8_t *)t->pixels + (size_t)y * t->pitch);
}

static uint32_t get_pixel(const struct target *t, int x, int y)
{
    if (t->pixels != NULL) {
        return row_of(t, y)[x] & 0x00ffffffu;
    }

    lua_rawgeti(t->L, t->table, (lua_Integer)y * t->w + x + 1);

    {
        uint32_t v = (uint32_t)lua_tointeger(t->L, -1);

        lua_pop(t->L, 1);

        return v & 0x00ffffffu;
    }
}

static void put_pixel(const struct target *t, int x, int y, uint32_t rgb)
{
    if (t->pixels != NULL) {
        /* A surface holds 0xAARRGGBB and is opaque; a table holds what the
         * portable renderer put there, which has no alpha byte at all. */
        row_of(t, y)[x] = rgb | 0xff000000u;
        return;
    }

    lua_pushinteger(t->L, (lua_Integer)rgb);
    lua_rawseti(t->L, t->table, (lua_Integer)y * t->w + x + 1);
}

/*
 * One pixel, blended - `soft.lua`'s `S:blend`, ported line for line.
 *
 * **Faithfulness is the requirement here, not improvement.** This is an
 * optional fast path under a renderer that also runs on other machines, so
 * a picture drawn with it must be the picture drawn without it. The
 * rounding is `floor` in the same three places, the channel arithmetic is
 * the same multiply and add, and where the Lua wrote `floor(d / 65536)`
 * this shifts - which is the same number for a value that cannot be
 * negative.
 */
static void blend(const struct target *t, int x, int y,
                  double r, double g, double b, double a)
{
    uint32_t d;
    double dr, dg, db;

    if (x < 0 || y < 0 || x >= t->w || y >= t->h || a <= 0.0) {
        return;
    }

    if (a >= 1.0) {
        put_pixel(t, x, y, ((uint32_t)floor(r) << 16)
                           | ((uint32_t)floor(g) << 8)
                           | (uint32_t)floor(b));
        return;
    }

    d = get_pixel(t, x, y);
    dr = (double)((d >> 16) & 0xff);
    dg = (double)((d >> 8) & 0xff);
    db = (double)(d & 0xff);

    put_pixel(t, x, y, ((uint32_t)floor(dr + (r - dr) * a) << 16)
                       | ((uint32_t)floor(dg + (g - dg) * a) << 8)
                       | (uint32_t)floor(db + (b - db) * a));
}

/*
 * `game.clear(target, w, h, colour)`
 *
 * The cheapest thing here and the one worth the most: at 960x540 this is
 * half a million stores, and in the interpreter it was 11% of a frame.
 * On a surface it is a row fill; on a table it is `lua_rawseti`, which is
 * still a fraction of what the same loop costs in Lua.
 */
static int l_clear(lua_State *L)
{
    struct target t = target_of(L, 1, 2);
    uint32_t colour = (uint32_t)luaL_checkinteger(L, 4) & 0x00ffffffu;
    int x, y;

    if (t.pixels != NULL) {
        for (y = 0; y < t.h; y++) {
            uint32_t *row = row_of(&t, y);

            for (x = 0; x < t.w; x++) {
                row[x] = colour | 0xff000000u;
            }
        }

        return 0;
    }

    for (y = 0; y < t.h; y++) {
        for (x = 0; x < t.w; x++) {
            lua_pushinteger(L, (lua_Integer)colour);
            lua_rawseti(L, t.table, (lua_Integer)y * t.w + x + 1);
        }
    }

    return 0;
}

/*
 * `game.line(target, w, h, x0, y0, x1, y1, r, g, b, a)`
 *
 * An anti-aliased line: Liang-Barsky to the edges, then Xiaolin Wu's two
 * covered pixels per step. `soft.lua`'s `S:line`, ported with the same
 * clipping, the same choice of major axis and the same coverage, because
 * an orbit that bends differently under the fast path is a fast path
 * nobody can trust.
 */
static bool clip_edge(double p, double q, double *t0, double *t1)
{
    double t;

    if (p == 0.0) {
        return q >= 0.0;
    }

    t = q / p;

    if (p < 0.0) {
        if (t > *t1) return false;
        if (t > *t0) *t0 = t;
    } else {
        if (t < *t0) return false;
        if (t < *t1) *t1 = t;
    }

    return true;
}

static int l_line(lua_State *L)
{
    struct target t = target_of(L, 1, 2);
    double x0 = luaL_checknumber(L, 4);
    double y0 = luaL_checknumber(L, 5);
    double x1 = luaL_checknumber(L, 6);
    double y1 = luaL_checknumber(L, 7);
    double r = luaL_checknumber(L, 8);
    double g = luaL_checknumber(L, 9);
    double b = luaL_checknumber(L, 10);
    double a = luaL_optnumber(L, 11, 1.0);

    double w = (double)t.w - 1.0;
    double h = (double)t.h - 1.0;
    double dx = x1 - x0;
    double dy = y1 - y0;
    double t0 = 0.0, t1 = 1.0;
    double ox = x0, oy = y0;

    if (!(clip_edge(-dx, x0, &t0, &t1)
          && clip_edge(dx, w - x0, &t0, &t1)
          && clip_edge(-dy, y0, &t0, &t1)
          && clip_edge(dy, h - y0, &t0, &t1))) {
        return 0;
    }

    /* Both ends from the *original* start, as the Lua does: computing the
     * second from an already-moved first shortens every clipped line. */
    x0 = ox + t0 * dx;
    y0 = oy + t0 * dy;
    x1 = ox + t1 * dx;
    y1 = oy + t1 * dy;

    dx = x1 - x0;
    dy = y1 - y0;

    if (fabs(dx) >= fabs(dy)) {
        double grad, y;
        long xs, xe, x;

        if (x0 > x1) {
            double sx = x0, sy = y0;

            x0 = x1; y0 = y1; x1 = sx; y1 = sy;
        }

        grad = (dx == 0.0) ? 0.0 : (y1 - y0) / (x1 - x0);
        xs = (long)floor(x0 + 0.5);
        xe = (long)floor(x1 + 0.5);
        y = y0 + grad * ((double)xs - x0);

        for (x = xs; x <= xe; x++) {
            double yi = floor(y);
            double f = y - yi;

            blend(&t, (int)x, (int)yi, r, g, b, a * (1.0 - f));
            blend(&t, (int)x, (int)yi + 1, r, g, b, a * f);
            y += grad;
        }

        return 0;
    }

    {
        double grad, x;
        long ys, ye, y;

        if (y0 > y1) {
            double sx = x0, sy = y0;

            x0 = x1; y0 = y1; x1 = sx; y1 = sy;
        }

        grad = (y1 - y0 == 0.0) ? 0.0 : (x1 - x0) / (y1 - y0);
        ys = (long)floor(y0 + 0.5);
        ye = (long)floor(y1 + 0.5);
        x = x0 + grad * ((double)ys - y0);

        for (y = ys; y <= ye; y++) {
            double xi = floor(x);
            double f = x - xi;

            blend(&t, (int)xi, (int)y, r, g, b, a * (1.0 - f));
            blend(&t, (int)xi + 1, (int)y, r, g, b, a * f);
            x += grad;
        }
    }

    return 0;
}

/*
 * The full rasterizer, in `gamesoft.c`: a surface and every loop that
 * fills it, `game.soft` for a program that wants one object rather than
 * these two free functions.
 *
 * It is a separate file because the two are answers to different
 * questions. What is here draws into *whatever a caller already has*,
 * including a Lua table, and is the cheap way to make an existing program
 * faster without changing what its framebuffer is. What is there owns a
 * surface, which is eighteen times quicker and is what a program written
 * for Kosmos should reach for.
 */
void kosmos_gamesoft_open(lua_State *L);

void kosmos_game_kit(lua_State *L)
{
    static const luaL_Reg api[] = {
        { "clear", l_clear },
        { "line",  l_line },
        { NULL, NULL }
    };

    luaL_newlib(L, api);

    kosmos_gamesoft_open(L);
    lua_setfield(L, -2, "soft");
}
