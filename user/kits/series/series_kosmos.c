/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Series Kit's Lua door** - `use("/Kosmos/Kits/series")` - a history
 * of numbers kept in C and drawn by C (`docs/sysmonitor.md`, S1).
 *
 *   local series = use("/Kosmos/Kits/series")
 *   local h = series.new{ series = 2 }          the pace only: 600 columns
 *   local h = series.new{ series = 2, hz = counter_hz, pace = ticks,
 *                         columns = 600, ten = 360, sixty = 1440,
 *                         names = { "kernel", "user" }, units = { "%", "%" } }
 *                                               and the 10 s and 60 s steps
 *   h:push({ k, u }, now)       a sample; a nil in it is a gap. `now` the
 *                               counter (`sys.ticks()`), needed with `hz`
 *   h:tick(now)                 the coarser steps brought up to `now`
 *   h:clear()                   every step emptied
 *   h:series()                  how many series
 *   h:capacity(step)            how many columns step 0, 1 or 2 holds
 *   h:written(step)             how many it has ever written
 *   h:column(step, index)       -> means, highests, at | nil: column `index`,
 *                               counted from 0, while it is held
 *   h:most(step, upto, count)   the largest highest of those columns
 *   h:draw(surface, x, y, w, h, spec) -> most
 *                               the columns into a surface (spec below)
 *   h:paint(surface, op)        a `series` drawing command, for `paint.lua`
 *
 * `spec`, and a command's fields beside its `x`, `y`, `w`, `h`:
 *
 *   mode     "stacked" (unsaid), "mirrored", "line"
 *   step     0, 1 or 2 (0)
 *   columns  how many fill the width (60)
 *   count    how many of the newest are drawn (all that are held)
 *   upto     drawn up to this column (`written`)
 *   most     the mirrored and line scale (mirrored: the busiest drawn;
 *            line: 100)
 *   colours  one a series, numbers
 *   cx, cy, cw, ch   the clip in force (none)
 *   pct      the scale it is drawn at (100)
 *
 * **The ground and the centre line are not drawn here.** They are one
 * rectangle each, and `ui.history.draw` draws them the way it always did;
 * what moved into C is the loop over columns, which was sixty `g:fill`s a
 * graph and is now one command.
 *
 * **The memory is the userdata's own**: the keeper and its ring in one
 * block the collector holds as one object and never walks into.
 */

#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "kits/gfx/gfx_draw.h"
#include "seriesring.h"
#include "series_core.h"

#define SERIES_MT "kosmos.series"

static struct series *check(lua_State *L)
{
    return *(struct series **)luaL_checkudata(L, 1, SERIES_MT);
}

/* A field of the table at `t` as an integer, or `otherwise`. */
static lua_Integer field_integer(lua_State *L, int t, const char *name, lua_Integer otherwise)
{
    lua_Integer v = otherwise;
    int isnum = 0;

    lua_getfield(L, t, name);
    if (!lua_isnil(L, -1)) {
        v = lua_tointegerx(L, -1, &isnum);
        if (!isnum) {
            luaL_error(L, "series: %s is a whole number", name);
        }
    }
    lua_pop(L, 1);
    return v;
}

static void copy_label(lua_State *L, int t, const char *list, uint32_t i, char *into, size_t room)
{
    lua_getfield(L, t, list);
    if (lua_istable(L, -1)) {
        lua_geti(L, -1, (lua_Integer)i + 1);
        if (lua_type(L, -1) == LUA_TSTRING) {
            size_t n;
            const char *s = lua_tolstring(L, -1, &n);

            if (n >= room) {
                n = room - 1;
            }
            memcpy(into, s, n);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static int l_new(lua_State *L)
{
    struct series_config c;
    struct series **box;
    struct series_ring *r;
    struct series *s;
    void *memory;
    size_t bytes;
    uint32_t i;

    luaL_checktype(L, 1, LUA_TTABLE);

    memset(&c, 0, sizeof c);
    c.series = (uint32_t)field_integer(L, 1, "series", 1);
    c.columns[0] = (uint32_t)field_integer(L, 1, "columns", SERIES_PACE_COLUMNS);
    c.columns[1] = (uint32_t)field_integer(L, 1, "ten", SERIES_TEN_COLUMNS);
    c.columns[2] = (uint32_t)field_integer(L, 1, "sixty", SERIES_SIXTY_COLUMNS);
    c.counter_hz = (uint64_t)field_integer(L, 1, "hz", 0);
    c.pace_counter_ticks = (uint64_t)field_integer(L, 1, "pace", 0);

    if (c.counter_hz == 0) {
        c.columns[1] = c.columns[2] = 0;
    }

    bytes = series_size(&c);
    if (bytes == 0) {
        return luaL_error(L, "series: %d series of %d columns is not a shape a ring has",
                          (int)c.series, (int)c.columns[0]);
    }

    /* The keeper's pointer first, then the block it points into: the block
     * is 8-aligned whatever Lua's own alignment of a userdata is. */
    box = (struct series **)lua_newuserdatauv(L, sizeof(*box) + bytes + 8, 0);
    memory = (void *)(((uintptr_t)(box + 1) + 7u) & ~(uintptr_t)7u);
    s = series_make(memory, &c);
    if (s == NULL) {
        return luaL_error(L, "series: the ring could not be made");
    }
    *box = s;

    r = series_ring_of(s);
    for (i = 0; i < c.series; i++) {
        struct series_label *l = &series_ring_labels(r)[i];

        copy_label(L, 1, "names", i, l->name, sizeof l->name);
        copy_label(L, 1, "units", i, l->unit, sizeof l->unit);
    }

    luaL_getmetatable(L, SERIES_MT);
    lua_setmetatable(L, -2);
    return 1;
}

/* A sample's values from the table at `t`: a number, or a gap. */
static void values_of(lua_State *L, int t, uint32_t n, double *values)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        lua_geti(L, t, (lua_Integer)i + 1);
        values[i] = lua_type(L, -1) == LUA_TNUMBER ? (double)lua_tonumber(L, -1)
                                                   : __builtin_nan("");
        lua_pop(L, 1);
    }
}

static int l_push(lua_State *L)
{
    struct series *s = check(L);
    const struct series_ring *r = series_ring_of_c(s);
    double values[SERIES_MAX];
    lua_Integer now;

    luaL_checktype(L, 2, LUA_TTABLE);
    now = luaL_optinteger(L, 3, 0);

    if (r->counter_hz != 0 && lua_isnoneornil(L, 3)) {
        return luaL_error(L, "series: a ring with coarser steps is pushed with the counter");
    }

    values_of(L, 2, r->series, values);
    series_push(s, values, (uint64_t)now);
    return 0;
}

static int l_tick(lua_State *L)
{
    series_tick(check(L), (uint64_t)luaL_checkinteger(L, 2));
    return 0;
}

static int l_clear(lua_State *L)
{
    series_clear(check(L));
    return 0;
}

static unsigned step_arg(lua_State *L, int index)
{
    lua_Integer k = luaL_optinteger(L, index, 0);

    luaL_argcheck(L, k >= 0 && k < (lua_Integer)SERIES_STEPS, index, "a step is 0, 1 or 2");
    return (unsigned)k;
}

static int l_series(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)series_ring_of_c(check(L))->series);
    return 1;
}

static int l_capacity(lua_State *L)
{
    const struct series_ring *r = series_ring_of_c(check(L));

    lua_pushinteger(L, (lua_Integer)r->step[step_arg(L, 2)].columns);
    return 1;
}

static int l_written(lua_State *L)
{
    const struct series_ring *r = series_ring_of_c(check(L));

    lua_pushinteger(L, (lua_Integer)series_ring_written(r, step_arg(L, 2)));
    return 1;
}

/* What step `k` holds now, newest last: (written, how many). */
static void held(const struct series_ring *r, unsigned k, uint64_t *upto, uint64_t *count)
{
    uint64_t written = series_ring_written(r, k);

    *upto = written;
    *count = written < r->step[k].columns ? written : r->step[k].columns;
}

static int l_column(lua_State *L)
{
    struct series *s = check(L);
    const struct series_ring *r = series_ring_of_c(s);
    unsigned k = step_arg(L, 2);
    lua_Integer index = luaL_checkinteger(L, 3);
    struct series_pair pairs[SERIES_MAX];
    uint64_t at;
    uint32_t i;

    if (index < 0 || !series_ring_copy(r, k, (uint64_t)index, pairs, &at)) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, (int)r->series, 0);
    lua_createtable(L, (int)r->series, 0);

    for (i = 0; i < r->series; i++) {
        if (pairs[i].mean == pairs[i].mean) {
            lua_pushnumber(L, (lua_Number)pairs[i].mean);
            lua_seti(L, -3, (lua_Integer)i + 1);
            lua_pushnumber(L, (lua_Number)pairs[i].high);
            lua_seti(L, -2, (lua_Integer)i + 1);
        }
    }

    lua_pushinteger(L, (lua_Integer)at);
    return 3;
}

static int l_most(lua_State *L)
{
    struct series *s = check(L);
    unsigned k = step_arg(L, 2);
    uint64_t upto, count;

    held(series_ring_of_c(s), k, &upto, &count);
    upto = (uint64_t)luaL_optinteger(L, 3, (lua_Integer)upto);
    count = (uint64_t)luaL_optinteger(L, 4, (lua_Integer)count);

    lua_pushnumber(L, (lua_Number)series_most(s, k, upto, count));
    return 1;
}

/* ------------------------------------------------------------ the drawing */

/*
 * The look and the place from the table at `t` - a `spec`, or a drawing
 * command - with `colours` as the room the look's colours are read into.
 */
static void read_look(lua_State *L, int t, struct series *s, struct series_look *look,
                      struct series_place *place, uint32_t *colours)
{
    const struct series_ring *r = series_ring_of_c(s);
    const char *mode;
    uint64_t upto, count;
    uint32_t i;

    memset(look, 0, sizeof *look);

    lua_getfield(L, t, "mode");
    mode = lua_tostring(L, -1);
    look->mode = mode != NULL && strcmp(mode, "mirrored") == 0 ? SERIES_MIRRORED
                 : mode != NULL && strcmp(mode, "line") == 0 ? SERIES_LINE
                 : SERIES_STACKED;
    lua_pop(L, 1);

    look->step = (unsigned)field_integer(L, t, "step", 0);
    if (look->step >= SERIES_STEPS) {
        luaL_error(L, "series: a step is 0, 1 or 2");
    }

    held(r, look->step, &upto, &count);
    look->columns = (long)field_integer(L, t, "columns", 60);
    look->upto = (uint64_t)field_integer(L, t, "upto", (lua_Integer)upto);
    look->count = (uint64_t)field_integer(L, t, "count", (lua_Integer)count);

    lua_getfield(L, t, "most");
    if (lua_type(L, -1) == LUA_TNUMBER) {
        look->most = (double)lua_tonumber(L, -1);
    } else if (look->mode == SERIES_MIRRORED) {
        look->most = series_most(s, look->step, look->upto, look->count);
    } else {
        look->most = 100;
    }
    lua_pop(L, 1);

    lua_getfield(L, t, "colours");
    for (i = 0; i < r->series; i++) {
        int isnum = 0;

        colours[i] = 0xff000000u;
        if (lua_istable(L, -1)) {
            lua_Integer c;

            lua_geti(L, -1, (lua_Integer)i + 1);
            c = lua_tointegerx(L, -1, &isnum);
            if (isnum) {
                colours[i] = (uint32_t)c;
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    look->colours = colours;
    look->ncolours = r->series;

    /* No clip said is a clip as large as `gc`'s own, which is no clip. */
    place->cx = (long)field_integer(L, t, "cx", -(1L << 40));
    place->cy = (long)field_integer(L, t, "cy", -(1L << 40));
    place->cw = (long)field_integer(L, t, "cw", 1L << 41);
    place->ch = (long)field_integer(L, t, "ch", 1L << 41);

    lua_getfield(L, t, "pct");
    place->pct = lua_type(L, -1) == LUA_TNUMBER ? (double)lua_tonumber(L, -1) : 100;
    lua_pop(L, 1);
}

static void fill_surface(void *ctx, long x, long y, long w, long h, uint32_t colour)
{
    gfx_draw_fill((struct surface *)ctx, x, y, w, h, colour);
}

static int l_draw(lua_State *L)
{
    struct series *s = check(L);
    struct surface *into = gfx_surface_check(L, 2);
    long x = (long)luaL_checkinteger(L, 3);
    long y = (long)luaL_checkinteger(L, 4);
    long w = (long)luaL_checkinteger(L, 5);
    long h = (long)luaL_checkinteger(L, 6);
    uint32_t colours[SERIES_MAX];
    struct series_look look;
    struct series_place place;

    lua_settop(L, 7);
    if (lua_isnil(L, 7)) {
        lua_newtable(L);
        lua_replace(L, 7);
    }
    luaL_checktype(L, 7, LUA_TTABLE);

    read_look(L, 7, s, &look, &place, colours);
    series_draw(s, x, y, w, h, &look, &place, fill_surface, into);

    lua_pushnumber(L, (lua_Number)look.most);
    return 1;
}

static int l_paint(lua_State *L)
{
    struct series *s = check(L);
    struct surface *into = gfx_surface_check(L, 2);
    uint32_t colours[SERIES_MAX];
    struct series_look look;
    struct series_place place;

    luaL_checktype(L, 3, LUA_TTABLE);
    read_look(L, 3, s, &look, &place, colours);
    series_draw(s, (long)field_integer(L, 3, "x", 0), (long)field_integer(L, 3, "y", 0),
                (long)field_integer(L, 3, "w", 0), (long)field_integer(L, 3, "h", 0),
                &look, &place, fill_surface, into);
    return 0;
}

void kosmos_series_kit(lua_State *L)
{
    static const luaL_Reg methods[] = {
        { "push",     l_push },
        { "tick",     l_tick },
        { "clear",    l_clear },
        { "series",   l_series },
        { "capacity", l_capacity },
        { "written",  l_written },
        { "column",   l_column },
        { "most",     l_most },
        { "draw",     l_draw },
        { "paint",    l_paint },
        { NULL, NULL }
    };

    static const luaL_Reg api[] = {
        { "new", l_new },
        { NULL, NULL }
    };

    if (luaL_newmetatable(L, SERIES_MT)) {
        luaL_newlib(L, methods);
        lua_setfield(L, -2, "__index");
    }

    lua_pop(L, 1);

    luaL_newlib(L, api);
}
