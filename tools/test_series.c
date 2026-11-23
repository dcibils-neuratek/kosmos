/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Series Kit on this machine** (`docs/sysmonitor.md` S1): its rings
 * and its drawing, held without booting anything.
 *
 *   - **the rings**: a push a column, the oldest written over and refused
 *     once it is, the 10 s and 60 s steps' means and highests against
 *     numbers worked out by hand on a fake clock, gaps - a series that did
 *     not answer, and columns no sample reached - and a counter that went
 *     back;
 *   - **the drawing, pixel for pixel**: `ui.history.draw` as it was in Lua
 *     before S1 - copied into this file, with the drawing context and
 *     `paint.lua`'s scale it went through - against the door onto the kit,
 *     every fill in the same order and every pixel the same, over four
 *     hundred histories stacked and mirrored, at eight sizes, in views
 *     that clip them and at five scales;
 *   - **the door**: `user/lib/ui.lua`'s `ui.history`, read out of the file
 *     once it is the kit's door, and this file's copy of it until then;
 *   - **its controls**: a graph drawn a column off, and one two pixels
 *     short, must be seen to differ, or the comparison could not have
 *     failed either.
 *
 * The kit's Lua door is compiled here as it is in Kosmos, against a
 * surface of this file's own that records every fill it is handed: the two
 * functions of `gfx_draw.h` it calls are defined below.
 *
 * Usage: test_series (from the top of the tree, for `user/lib/ui.lua`)
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "kits/gfx/gfx_draw.h"
#include "seriesring.h"
#include "series_core.h"

void kosmos_series_kit(lua_State *L);

static int failures, checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("not ok %d - %s\n", checks, what);
    }
}

static int near(double a, double b)
{
    return fabs(a - b) < 1e-9;
}

/* ----------------------------------------------------------- the rings */

static struct series *make(const struct series_config *c)
{
    size_t bytes = series_size(c);
    void *memory = bytes ? aligned_alloc(64, (bytes + 63) & ~(size_t)63) : NULL;

    return memory ? series_make(memory, c) : NULL;
}

static int column(struct series *s, unsigned k, uint64_t index, struct series_pair *out,
                  uint64_t *at)
{
    return series_ring_copy(series_ring_of_c(s), k, index, out, at);
}

static void rings(void)
{
    struct series_pair p[4];
    uint64_t at;
    double v[2];
    int i;

    /* **Push, and the oldest let go**: five columns, seven pushed. */
    {
        struct series_config c = { 2, { 5, 360, 1440 }, 0, 0 };
        struct series *s = make(&c);
        const struct series_ring *r = series_ring_of_c(s);

        check(s != NULL, "a ring of two series and five columns is made");
        check(r->step[1].columns == 0 && r->step[2].columns == 0,
              "without a counter there are no coarser steps");

        for (i = 0; i < 7; i++) {
            v[0] = i;
            v[1] = 10 * i;
            series_push(s, v, (uint64_t)i);
        }

        check(series_ring_written(r, 0) == 7, "seven pushes, seven columns written");
        check(!column(s, 0, 0, p, NULL) && !column(s, 0, 1, p, NULL),
              "the two oldest are written over, and refused");
        check(column(s, 0, 2, p, &at) && p[0].mean == 2 && p[1].high == 20 && at == 2,
              "the oldest held is the third, its mean and highest the sample");
        check(column(s, 0, 6, p, NULL) && p[0].mean == 6 && p[1].mean == 60,
              "the newest is the seventh");
        check(!column(s, 0, 7, p, NULL), "a column not yet written is refused");
        check(series_most(s, 0, 7, 5) == 60, "the most of what is held: 60");
        check(series_most(s, 0, 7, 2) == 60 && series_most(s, 0, 6, 2) == 50,
              "the most of the newest two, and of the two before the last");

        /* A gap in the first series stops a column's most there, as
         * `ipairs` stopped at a sample's first nil. */
        v[0] = NAN;
        v[1] = 1000;
        series_push(s, v, 7);
        check(column(s, 0, 7, p, NULL) && isnan(p[0].mean) && isnan(p[0].high)
              && p[1].mean == 1000, "a series with no answer is a gap, the other kept");
        check(series_most(s, 0, 8, 1) == 0, "a column whose first series is a gap adds nothing");

        series_clear(s);
        check(series_ring_written(r, 0) == 0 && !column(s, 0, 0, p, NULL), "cleared, nothing held");
        free(s);
    }

    /*
     * **The three steps on a fake clock**: a counter of 1000 a second, a
     * sample a second for 130 seconds, the first series `i` and the second
     * `(7 i) % 13`. Worked by hand:
     *
     *   10 s column 0: i 0..9, mean 4.5, highest 9; the second series
     *   0 7 1 8 2 9 3 10 4 11, sum 55, mean 5.5, highest 11.
     *   10 s column 12 is open at 129 s, so twelve are written.
     *   60 s column 0: mean 29.5, highest 59; column 1: 89.5, 119.
     */
    {
        struct series_config c = { 2, { 600, 360, 1440 }, 1000, 1000 };
        struct series *s = make(&c);
        const struct series_ring *r = series_ring_of_c(s);
        int bad = 0;

        for (i = 0; i < 130; i++) {
            v[0] = i;
            v[1] = (7 * i) % 13;
            series_push(s, v, (uint64_t)i * 1000);
        }

        check(series_ring_written(r, 0) == 130, "a column a push at the pace");
        check(series_ring_written(r, 1) == 12, "twelve ten-second columns closed, the thirteenth open");
        check(series_ring_written(r, 2) == 2, "two minute columns closed");

        check(column(s, 1, 0, p, &at) && near(p[0].mean, 4.5) && p[0].high == 9
              && near(p[1].mean, 5.5) && p[1].high == 11 && at == 0,
              "10 s column 0: 4.5 and 9, 5.5 and 11, from the counter's 0");
        check(column(s, 1, 7, p, &at) && near(p[0].mean, 74.5) && p[0].high == 79 && at == 70000,
              "10 s column 7: 74.5 and 79, at 70 s");
        check(column(s, 2, 0, p, &at) && near(p[0].mean, 29.5) && p[0].high == 59 && at == 0,
              "60 s column 0: 29.5 and 59");
        check(column(s, 2, 1, p, &at) && near(p[0].mean, 89.5) && p[0].high == 119 && at == 60000,
              "60 s column 1: 89.5 and 119, at 60 s");

        /* And every ten-second column's second series against a plain sum. */
        for (i = 0; i < 12; i++) {
            double sum = 0, high = 0;
            int j;

            for (j = 10 * i; j < 10 * i + 10; j++) {
                sum += (7 * j) % 13;
                if ((7 * j) % 13 > high) {
                    high = (7 * j) % 13;
                }
            }
            if (!column(s, 1, (uint64_t)i, p, NULL) || !near(p[1].mean, sum / 10)
                || p[1].high != high) {
                bad++;
            }
        }
        check(bad == 0, "every 10 s column's mean and highest, against a plain sum");

        /* A counter that went back is counted in the open column. */
        v[0] = 1000;
        v[1] = 0;
        series_push(s, v, 115000);
        series_tick(s, 140000);
        check(series_ring_written(r, 1) == 14 && column(s, 1, 12, p, NULL)
              && near(p[0].mean, (120 + 121 + 122 + 123 + 124 + 125 + 126 + 127 + 128 + 129
                                  + 1000) / 11.0) && p[0].high == 1000,
              "a sample stamped back in time joins the open column; the tick closes it and 130 s");
        free(s);
    }

    /*
     * **Gaps**: a source that answers for ten seconds, is silent until
     * 45 s, answers once, and is silent again; and a series that never
     * answers in a column.
     */
    {
        struct series_config c = { 2, { 600, 6, 1440 }, 1000, 1000 };
        struct series *s = make(&c);
        const struct series_ring *r = series_ring_of_c(s);

        for (i = 0; i < 10; i++) {
            v[0] = 5;
            v[1] = NAN;
            series_push(s, v, (uint64_t)i * 1000);
        }

        v[0] = 9;
        v[1] = 3;
        series_push(s, v, 45000);

        check(series_ring_written(r, 1) == 4, "the first column and three gaps written at 45 s");
        check(column(s, 1, 0, p, NULL) && p[0].mean == 5 && isnan(p[1].mean) && isnan(p[1].high),
              "a series that never answered in a column is a gap there, the other kept");
        check(column(s, 1, 1, p, &at) && isnan(p[0].mean) && isnan(p[1].mean) && at == 10000,
              "a column no sample reached is a gap, at its own time");
        check(column(s, 1, 3, p, &at) && isnan(p[0].mean) && at == 30000, "and so on to 30 s");

        series_tick(s, 75000);
        check(series_ring_written(r, 1) == 7 && column(s, 1, 4, p, &at) && p[0].mean == 9
              && p[1].mean == 3 && at == 40000, "the tick closes 40 s");
        check(column(s, 1, 6, p, &at) && isnan(p[0].mean) && at == 60000,
              "and writes 50 s and 60 s as gaps");

        /* Silent for longer than the step holds: only what it holds. */
        series_tick(s, 1000000);
        check(series_ring_written(r, 1) == 7 + 6, "a long silence writes the ring's six, not ninety");
        check(column(s, 1, 12, p, &at) && isnan(p[0].mean) && at == 990000,
              "the newest gap is the column before now");

        v[0] = 1;
        v[1] = 1;
        series_push(s, v, 1000500);
        series_tick(s, 1010000);
        check(series_ring_written(r, 1) == 14 && column(s, 1, 13, p, &at) && p[0].mean == 1
              && at == 1000000, "and the next sample opens the column of now");
        free(s);
    }

    /* **The shape**: what a ring of eighty series at the defaults costs. */
    {
        uint32_t columns[SERIES_STEPS] = { SERIES_PACE_COLUMNS, SERIES_TEN_COLUMNS,
                                           SERIES_SIXTY_COLUMNS };
        uint64_t one = series_ring_bytes(1, columns);
        uint64_t eighty = series_ring_bytes(80, columns);

        printf("  a series at the defaults: %llu bytes; eighty: %llu\n",
               (unsigned long long)one, (unsigned long long)eighty);
        /*
         * By hand: the header and eighty labels, 128 + 2560 = 2688; then
         * each step's slots - its columns and the spare - at 8 bytes of
         * time and 80 pairs of 16, each step rounded up to 64:
         * 601 x 1288 = 774,088 -> 774,144; 361 x 1288 = 464,968 -> 465,024;
         * 1441 x 1288 = 1,856,008 -> 1,856,064. 3,097,920 in all.
         */
        check(eighty == 3097920, "eighty series at the defaults: 3,097,920 bytes, as worked by hand");
        check(series_ring_bytes(0, columns) == 0 && series_ring_bytes(SERIES_MAX + 1, columns) == 0,
              "no ring of no series, nor of more than the most");
    }
}

/* ----------------------------------------------- a surface of our own */

/*
 * `gfx_draw.h` keeps `struct surface` opaque from every kit, this one's
 * door included, so the test's own definition is the only one it meets.
 */
struct surface {
    long      w, h;
    uint32_t *px;
    char     *log;
    size_t    len, cap;
};

#define SURFACE_MT "test.surface"

static void note(struct surface *s, const char *line)
{
    size_t n = strlen(line);

    if (s->len + n + 1 > s->cap) {
        s->cap = (s->len + n + 1) * 2;
        s->log = realloc(s->log, s->cap);
    }
    memcpy(s->log + s->len, line, n + 1);
    s->len += n;
}

/* A fill as gfx's `fill_on` does it - clipped to the surface - with the
 * call written down first, as it was made. */
static void fill(struct surface *s, long x, long y, long w, long h, uint32_t colour)
{
    char line[96];
    long r, c;

    snprintf(line, sizeof line, "F %ld %ld %ld %ld %08x\n", x, y, w, h, colour);
    note(s, line);

    for (r = y < 0 ? 0 : y; r < y + h && r < s->h; r++) {
        for (c = x < 0 ? 0 : x; c < x + w && c < s->w; c++) {
            s->px[r * s->w + c] = colour;
        }
    }
}

struct surface *gfx_surface_check(struct lua_State *L, int index)
{
    return luaL_checkudata(L, index, SURFACE_MT);
}

void gfx_draw_fill(struct surface *s, long x, long y, long w, long h, uint32_t colour)
{
    fill(s, x, y, w, h, colour);
}

static int l_surface(lua_State *L)
{
    long w = (long)luaL_checkinteger(L, 1), h = (long)luaL_checkinteger(L, 2);
    struct surface *s = lua_newuserdatauv(L, sizeof *s, 0);

    memset(s, 0, sizeof *s);
    s->w = w;
    s->h = h;
    s->px = calloc((size_t)(w * h), sizeof(uint32_t));
    luaL_setmetatable(L, SURFACE_MT);
    return 1;
}

/* `s:fill`, as gfx's binding takes it: whole numbers, or an error. */
static int l_fill(lua_State *L)
{
    fill(luaL_checkudata(L, 1, SURFACE_MT), (long)luaL_checkinteger(L, 2),
         (long)luaL_checkinteger(L, 3), (long)luaL_checkinteger(L, 4),
         (long)luaL_checkinteger(L, 5), (uint32_t)luaL_checkinteger(L, 6));
    return 0;
}

/* A rounded ground is drawn the same way by both paths - it is the same
 * Lua `g:fill_round` - so it is written down, and filled square. */
static int l_fill_round(lua_State *L)
{
    struct surface *s = luaL_checkudata(L, 1, SURFACE_MT);
    char line[160];

    snprintf(line, sizeof line, "R %lld %lld %lld %lld %08llx %lld %lld %lld %lld %lld\n",
             (long long)luaL_checkinteger(L, 2), (long long)luaL_checkinteger(L, 3),
             (long long)luaL_checkinteger(L, 4), (long long)luaL_checkinteger(L, 5),
             (long long)luaL_checkinteger(L, 6), (long long)luaL_optinteger(L, 7, 0),
             (long long)luaL_optinteger(L, 8, -1), (long long)luaL_optinteger(L, 9, -1),
             (long long)luaL_optinteger(L, 10, -1), (long long)luaL_optinteger(L, 11, -1));
    note(s, line);
    return 0;
}

static int l_log(lua_State *L)
{
    struct surface *s = luaL_checkudata(L, 1, SURFACE_MT);

    lua_pushlstring(L, s->log ? s->log : "", s->len);
    return 1;
}

static int l_pixels(lua_State *L)
{
    struct surface *s = luaL_checkudata(L, 1, SURFACE_MT);

    lua_pushlstring(L, (const char *)s->px, (size_t)(s->w * s->h) * sizeof(uint32_t));
    return 1;
}

static int l_at(lua_State *L)
{
    struct surface *s = luaL_checkudata(L, 1, SURFACE_MT);
    long x = (long)luaL_checkinteger(L, 2), y = (long)luaL_checkinteger(L, 3);

    lua_pushinteger(L, x >= 0 && y >= 0 && x < s->w && y < s->h ? s->px[y * s->w + x] : 0);
    return 1;
}

/* The rows holding a pixel of `colour`, as a set. */
static int l_rows_of(lua_State *L)
{
    struct surface *s = luaL_checkudata(L, 1, SURFACE_MT);
    uint32_t colour = (uint32_t)luaL_checkinteger(L, 2);
    long r, c;

    lua_newtable(L);
    for (r = 0; r < s->h; r++) {
        for (c = 0; c < s->w; c++) {
            if (s->px[r * s->w + c] == colour) {
                lua_pushboolean(L, 1);
                lua_seti(L, -2, r);
                break;
            }
        }
    }
    return 1;
}

static int l_gc(lua_State *L)
{
    struct surface *s = luaL_checkudata(L, 1, SURFACE_MT);

    free(s->px);
    free(s->log);
    s->px = NULL;
    s->log = NULL;
    return 0;
}

static int l_check(lua_State *L)
{
    check(lua_toboolean(L, 1), luaL_checkstring(L, 2));
    return 0;
}

/* -------------------------------------------------------------- the door */

/*
 * `ui.history` from `user/lib/ui.lua` - from its table to the processor's
 * chip after it - when that is the kit's door; this file's copy before.
 */
static char *door_from_ui(void)
{
    FILE *f = fopen("user/lib/ui.lua", "rb");
    const char *from = "\nui.history = {}\n";
    const char *to = "\n--\n-- **A processor's chip";
    char *text, *a, *b, *door = NULL;
    long n;

    if (f == NULL) {
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = malloc((size_t)n + 1);
    if (text != NULL && fread(text, 1, (size_t)n, f) == (size_t)n) {
        text[n] = 0;
        a = strstr(text, from);
        b = a != NULL ? strstr(a, to) : NULL;
        if (b != NULL) {
            *b = 0;
            if (strstr(a, "/Kosmos/Kits/series") != NULL) {
                door = strdup(a + 1);
            }
        }
    }

    free(text);
    fclose(f);
    return door;
}

static const char DOOR_COPY[] =
    "ui.history = {}\n"
    "\n"
    "--\n"
    "-- **The Series Kit behind it** (`docs/sysmonitor.md` S1). A history is\n"
    "-- still the list its caller keeps and reads - a sample a table, the oldest\n"
    "-- let go past `columns` - so every monitor that reads its own list for a\n"
    "-- peak goes on doing so. What changed is the drawing: each list is\n"
    "-- mirrored into a ring in C (`/Kosmos/Kits/series`), and a graph is one\n"
    "-- drawing command whose columns the kit fills, where it was a `g:fill`\n"
    "-- from Lua a column. The ring follows the list while only `push` changes\n"
    "-- it; a list changed any other way, or made afresh for each draw, is read\n"
    "-- into its ring again when it is drawn.\n"
    "--\n"
    "local series_kit = nil\n"
    "local rings = setmetatable({}, { __mode = \"k\" })\n"
    "\n"
    "-- Whether a list's ring still holds what the list does.\n"
    "local function in_step(r, list)\n"
    "  local n = #list\n"
    "\n"
    "  return r.count == n and r.first == list[1] and r.last == list[n]\n"
    "end\n"
    "\n"
    "-- The ring `list` is drawn from, brought up to what the list holds.\n"
    "local function ring_of(list, columns)\n"
    "  local r = rings[list]\n"
    "  local n = #list\n"
    "\n"
    "  if r and in_step(r, list) then return r.ring end\n"
    "\n"
    "  local wide = 2\n"
    "\n"
    "  for i = 1, n do\n"
    "    if #list[i] > wide then wide = #list[i] end\n"
    "  end\n"
    "\n"
    "  if not r or r.ring:series() < wide or r.ring:capacity() < math.max(n, columns) then\n"
    "    series_kit = series_kit or use(\"/Kosmos/Kits/series\")\n"
    "    r = { ring = series_kit.new{ series = wide, columns = math.max(n, columns, 60) } }\n"
    "    rings[list] = r\n"
    "  else\n"
    "    r.ring:clear()\n"
    "  end\n"
    "\n"
    "  for i = 1, n do r.ring:push(list[i]) end\n"
    "\n"
    "  r.count, r.first, r.last = n, list[1], list[n]\n"
    "  return r.ring\n"
    "end\n"
    "\n"
    "--\n"
    "-- **A sample added to a history**, the oldest let go of past `columns`\n"
    "-- (sixty, a minute at a second's pace). A sample is a list of numbers, one\n"
    "-- a series; a nil in it is a gap.\n"
    "--\n"
    "function ui.history.push(list, sample, columns)\n"
    "  local r = rings[list]\n"
    "  local was = r and in_step(r, list)\n"
    "\n"
    "  list[#list + 1] = sample\n"
    "\n"
    "  while #list > (columns or 60) do table.remove(list, 1) end\n"
    "\n"
    "  if was and #sample <= r.ring:series() and #list <= r.ring:capacity() then\n"
    "    r.ring:push(sample)\n"
    "    r.count, r.first, r.last = #list, list[1], sample\n"
    "  elseif r then\n"
    "    r.count = -1\n"
    "  end\n"
    "\n"
    "  return list\n"
    "end\n"
    "\n"
    "--\n"
    "-- **The largest value in a history**, for a mirrored graph's scale: the\n"
    "-- busiest moment either way, so the graph is fitted to what it shows.\n"
    "--\n"
    "function ui.history.most(list)\n"
    "  local most = 0\n"
    "\n"
    "  for _, s in ipairs(list) do\n"
    "    for _, v in ipairs(s) do\n"
    "      if v > most then most = v end\n"
    "    end\n"
    "  end\n"
    "\n"
    "  return most\n"
    "end\n"
    "\n"
    "--\n"
    "-- **A history drawn**, the newest sample at the right edge and each a\n"
    "-- column - a sixtieth of the width for sixty - with a pixel of ground\n"
    "-- between two when there is room for one.\n"
    "--\n"
    "-- `spec`:\n"
    "--   colours  one a series\n"
    "--   mode     \"stacked\": percentages, the first series at the floor and each\n"
    "--            next on top of it - Monitor's kernel and user time;\n"
    "--            \"mirrored\": two series about a centre line, the first above\n"
    "--            it and the second below - in and out - on one scale;\n"
    "--            \"line\": each series a line, to `most` (100 unsaid)\n"
    "--   most     a mirrored graph's scale; its busiest value when not given\n"
    "--   columns  how many samples fill the width (60)\n"
    "--   ground   the graph's own colour behind the columns\n"
    "--   radius   its corners (8)\n"
    "--\n"
    "-- Returns the scale a mirrored graph was drawn to, for its caller to say.\n"
    "--\n"
    "-- The ground and the centre line are a rectangle each, as before; the\n"
    "-- columns are the kit's, drawn where `g:fill` would have put them - inside\n"
    "-- the clip in force, and at the window's scale by their two edges\n"
    "-- (`series_core.c`, `emit`), which `tools/test_series.c` holds to this\n"
    "-- function as it was in Lua, pixel for pixel.\n"
    "--\n"
    "function ui.history.draw(g, x, y, w, h, list, spec)\n"
    "  local columns = spec.columns or 60\n"
    "  local colours = spec.colours or { theme.io_in, theme.io_out }\n"
    "  local mirrored = spec.mode == \"mirrored\"\n"
    "  local line = spec.mode == \"line\"\n"
    "  local most = mirrored and (spec.most or ui.history.most(list)) or line and spec.most or 100\n"
    "\n"
    "  g:fill_round(x, y, w, h, spec.ground or theme.sunken, spec.radius or 8)\n"
    "\n"
    "  local inner = h - 4\n"
    "  local centre = y + 2 + inner // 2\n"
    "\n"
    "  if mirrored then g:fill(x + 2, centre, w - 4, 1, theme.line_soft) end\n"
    "  if most <= 0 or #list == 0 then return most end\n"
    "\n"
    "  local ring = ring_of(list, columns)\n"
    "  local shades = {}\n"
    "\n"
    "  for k = 1, ring:series() do shades[k] = shade(colours[k]) or 0xff000000 end\n"
    "\n"
    "  local o = { op = \"series\", ring = ring, x = x, y = y, w = w, h = h,\n"
    "              mode = mirrored and \"mirrored\" or line and \"line\" or \"stacked\",\n"
    "              columns = columns, most = most, count = #list, upto = ring:written(),\n"
    "              colours = shades }\n"
    "\n"
    "  if g.ops then\n"
    "    -- In the drawing context's own coordinates, and inside its clip, as\n"
    "    -- `gc:fill` would have put each column.\n"
    "    o.x, o.y = (g.ox or 0) + x, (g.oy or 0) + y\n"
    "    o.cx, o.cy, o.cw, o.ch = g.cx, g.cy, g.cw, g.ch\n"
    "    g.ops[#g.ops + 1] = o\n"
    "  else\n"
    "    ring:draw(g, x, y, w, h, o)\n"
    "  end\n"
    "\n"
    "  return most\n"
    "end\n"
    "\n";

static const char HELD[] =
    "-- Set by C: SERIES (the kit), surface(w, h), DOOR (the door's source and\n"
    "-- where it came from), check(ok, what).\n"
    "\n"
    "local theme = { io_in = 0xff4fb3ff, io_out = 0xfff0883e, sunken = 0xff161b22,\n"
    "                line_soft = 0xff30363d, accent = 0xff2f81f7 }\n"
    "\n"
    "local function shade(c)\n"
    "  if type(c) == \"string\" then return theme[c] end\n"
    "  return c\n"
    "end\n"
    "\n"
    "--------------------------------------------------------------------------\n"
    "-- **Today's drawing, as it was in Lua** - `ui.lua`'s graphics context and\n"
    "-- `ui.history` before S1, and `paint.lua`'s fill, fill_round and scale.\n"
    "-- Copied, not rewritten: this is the reference the kit is held to, and it\n"
    "-- must stay what Monitor drew on 23 November 2026 whatever ui.lua becomes.\n"
    "--------------------------------------------------------------------------\n"
    "\n"
    "local gc = {}\n"
    "gc.__index = gc\n"
    "\n"
    "local function new_gc()\n"
    "  return setmetatable({ ops = {}, ox = 0, oy = 0,\n"
    "                        cx = 0, cy = 0, cw = 1 << 30, ch = 1 << 30 }, gc)\n"
    "end\n"
    "\n"
    "function gc:push(x, y, w, h)\n"
    "  local saved = { self.ox, self.oy, self.cx, self.cy, self.cw, self.ch }\n"
    "\n"
    "  self.ox = self.ox + x\n"
    "  self.oy = self.oy + y\n"
    "\n"
    "  local nx, ny = self.ox, self.oy\n"
    "  local x0 = (nx > self.cx) and nx or self.cx\n"
    "  local y0 = (ny > self.cy) and ny or self.cy\n"
    "  local x1 = math.min(nx + w, self.cx + self.cw)\n"
    "  local y1 = math.min(ny + h, self.cy + self.ch)\n"
    "\n"
    "  self.cx, self.cy = x0, y0\n"
    "  self.cw = (x1 > x0) and (x1 - x0) or 0\n"
    "  self.ch = (y1 > y0) and (y1 - y0) or 0\n"
    "\n"
    "  return saved\n"
    "end\n"
    "\n"
    "function gc:fill(x, y, w, h, color)\n"
    "  color = shade(color)\n"
    "  local ax, ay = self.ox + x, self.oy + y\n"
    "\n"
    "  local x0 = (ax > self.cx) and ax or self.cx\n"
    "  local y0 = (ay > self.cy) and ay or self.cy\n"
    "  local x1 = math.min(ax + w, self.cx + self.cw)\n"
    "  local y1 = math.min(ay + h, self.cy + self.ch)\n"
    "\n"
    "  if x1 <= x0 or y1 <= y0 then return end\n"
    "\n"
    "  self.ops[#self.ops + 1] = { op = \"fill\", x = x0, y = y0,\n"
    "                              w = x1 - x0, h = y1 - y0, color = color }\n"
    "end\n"
    "\n"
    "local function round_op(self, kind, x, y, w, h, color, r)\n"
    "  if w <= 0 or h <= 0 then return end\n"
    "\n"
    "  local ax, ay = self.ox + x, self.oy + y\n"
    "\n"
    "  if self.cw and (ax >= self.cx + self.cw or ay >= self.cy + self.ch\n"
    "                  or ax + w <= self.cx or ay + h <= self.cy) then\n"
    "    return\n"
    "  end\n"
    "\n"
    "  local o = { op = kind, x = ax, y = ay, w = w, h = h, color = shade(color), r = r or 0 }\n"
    "\n"
    "  if self.cw and (ax < self.cx or ay < self.cy or ax + w > self.cx + self.cw\n"
    "                  or ay + h > self.cy + self.ch) then\n"
    "    o.cx, o.cy, o.cw, o.ch = self.cx, self.cy, self.cw, self.ch\n"
    "  end\n"
    "\n"
    "  self.ops[#self.ops + 1] = o\n"
    "end\n"
    "\n"
    "function gc:fill_round(x, y, w, h, color, r)\n"
    "  round_op(self, \"fill_round\", x, y, w, h, color, r)\n"
    "end\n"
    "\n"
    "local ref = {}\n"
    "\n"
    "function ref.push(list, sample, columns)\n"
    "  list[#list + 1] = sample\n"
    "\n"
    "  while #list > (columns or 60) do table.remove(list, 1) end\n"
    "\n"
    "  return list\n"
    "end\n"
    "\n"
    "function ref.most(list)\n"
    "  local most = 0\n"
    "\n"
    "  for _, s in ipairs(list) do\n"
    "    for _, v in ipairs(s) do\n"
    "      if v > most then most = v end\n"
    "    end\n"
    "  end\n"
    "\n"
    "  return most\n"
    "end\n"
    "\n"
    "function ref.draw(g, x, y, w, h, list, spec)\n"
    "  local columns = spec.columns or 60\n"
    "  local colours = spec.colours or { theme.io_in, theme.io_out }\n"
    "  local mirrored = spec.mode == \"mirrored\"\n"
    "  local most = mirrored and (spec.most or ref.most(list)) or 100\n"
    "\n"
    "  g:fill_round(x, y, w, h, spec.ground or theme.sunken, spec.radius or 8)\n"
    "\n"
    "  local inner = h - 4\n"
    "  local base = y + h - 2\n"
    "  local centre = y + 2 + inner // 2\n"
    "\n"
    "  if mirrored then g:fill(x + 2, centre, w - 4, 1, theme.line_soft) end\n"
    "  if most <= 0 then return most end\n"
    "\n"
    "  for i = 1, #list do\n"
    "    local slot = columns - #list + i - 1\n"
    "    local x0 = x + 2 + (slot * (w - 4)) // columns\n"
    "    local x1 = x + 2 + ((slot + 1) * (w - 4)) // columns\n"
    "    local cw = math.max(1, x1 - x0 - 1)\n"
    "    local s = list[i]\n"
    "\n"
    "    if mirrored then\n"
    "      local half = inner // 2\n"
    "      local up = math.min(half, math.floor(half * (s[1] or 0) / most + 0.5))\n"
    "      local down = math.min(half, math.floor(half * (s[2] or 0) / most + 0.5))\n"
    "\n"
    "      if up > 0 then g:fill(x0, centre - up, cw, up, colours[1]) end\n"
    "      if down > 0 then g:fill(x0, centre + 1, cw, down, colours[2]) end\n"
    "    else\n"
    "      local at = base\n"
    "\n"
    "      for k, v in ipairs(s) do\n"
    "        local part = (inner * v) // 100\n"
    "\n"
    "        if part > 0 then\n"
    "          g:fill(x0, at - part, cw, part, colours[k])\n"
    "          at = at - part\n"
    "        end\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "\n"
    "  return most\n"
    "end\n"
    "\n"
    "local function px(v, pct)\n"
    "  return math.floor(v * pct / 100 + 0.5)\n"
    "end\n"
    "\n"
    "local function scale(o, pct)\n"
    "  local kind = o.op\n"
    "\n"
    "  if kind == \"fill\" or kind == \"wash\" or kind == \"fill_round\" or kind == \"frame_round\" then\n"
    "    local x, y = tonumber(o.x) or 0, tonumber(o.y) or 0\n"
    "    local w, h = tonumber(o.w) or 0, tonumber(o.h) or 0\n"
    "    local x0, y0 = px(x, pct), px(y, pct)\n"
    "\n"
    "    o.x, o.y = x0, y0\n"
    "    o.w, o.h = px(x + w, pct) - x0, px(y + h, pct) - y0\n"
    "\n"
    "    if o.r then o.r = px(o.r, pct) end\n"
    "\n"
    "    if o.cx then\n"
    "      local cx, cy = tonumber(o.cx) or 0, tonumber(o.cy) or 0\n"
    "      local cw, ch = tonumber(o.cw) or 0, tonumber(o.ch) or 0\n"
    "      local cx0, cy0 = px(cx, pct), px(cy, pct)\n"
    "\n"
    "      o.cx, o.cy = cx0, cy0\n"
    "      o.cw, o.ch = px(cx + cw, pct) - cx0, px(cy + ch, pct) - cy0\n"
    "    end\n"
    "  -- S1's one line in `paint.scale`.\n"
    "  elseif kind == \"series\" then\n"
    "    o.pct = pct\n"
    "  end\n"
    "end\n"
    "\n"
    "local painter = {\n"
    "  fill = function(s, o)\n"
    "    s:fill(o.x or 0, o.y or 0, o.w or 0, o.h or 0, o.color or 0xff000000)\n"
    "  end,\n"
    "  fill_round = function(s, o)\n"
    "    s:fill_round(o.x or 0, o.y or 0, o.w or 0, o.h or 0,\n"
    "                 o.color or 0xff000000, o.r or 0, o.cx, o.cy, o.cw, o.ch)\n"
    "  end,\n"
    "  -- S1's handler in `paint.lua`.\n"
    "  series = function(s, o)\n"
    "    if o.ring then o.ring:paint(s, o) end\n"
    "  end,\n"
    "}\n"
    "\n"
    "local function paint_ops(surface, ops, pct)\n"
    "  if pct and pct ~= 100 then\n"
    "    for _, o in ipairs(ops) do scale(o, pct) end\n"
    "  end\n"
    "\n"
    "  for _, o in ipairs(ops) do\n"
    "    local fn = painter[o.op]\n"
    "\n"
    "    if fn then fn(surface, o) end\n"
    "  end\n"
    "end\n"
    "\n"
    "--------------------------------------------------------------------------\n"
    "-- **The door**, as `ui.lua` has it once S1 is applied, or this test's copy\n"
    "-- until then - with its counter of rings made.\n"
    "--------------------------------------------------------------------------\n"
    "\n"
    "local made = 0\n"
    "local kit = setmetatable({ new = function(t) made = made + 1; return SERIES.new(t) end },\n"
    "                         { __index = SERIES })\n"
    "local env = setmetatable({ ui = {}, theme = theme, shade = shade,\n"
    "                           use = function(path)\n"
    "                             assert(path == \"/Kosmos/Kits/series\", path)\n"
    "                             return kit\n"
    "                           end }, { __index = _G })\n"
    "\n"
    "assert(load(DOOR, \"door\", \"t\", env))()\n"
    "\n"
    "local door = env.ui.history\n"
    "\n"
    "--------------------------------------------------------------------------\n"
    "-- The cases.\n"
    "--------------------------------------------------------------------------\n"
    "\n"
    "local seed = 12345\n"
    "\n"
    "local function rand(n)\n"
    "  seed = (seed * 1103515245 + 12345) % 2147483648\n"
    "  return seed % n\n"
    "end\n"
    "\n"
    "local function copy(list)\n"
    "  local out = {}\n"
    "\n"
    "  for i, s in ipairs(list) do\n"
    "    local t = {}\n"
    "\n"
    "    for k = 1, 4 do t[k] = s[k] end\n"
    "    out[i] = t\n"
    "  end\n"
    "\n"
    "  return out\n"
    "end\n"
    "\n"
    "local W, H = 420, 220\n"
    "\n"
    "-- One graph drawn both ways at a place and a scale: whether every fill\n"
    "-- came out the same, in the same order, and the pixels with it.\n"
    "local function both(list_ref, list_new, x, y, w, h, spec, view, pct, spoil)\n"
    "  local a, b = surface(W, H), surface(W, H)\n"
    "  local ga, gb = new_gc(), new_gc()\n"
    "\n"
    "  if view then\n"
    "    ga:push(view[1], view[2], view[3], view[4])\n"
    "    gb:push(view[1], view[2], view[3], view[4])\n"
    "  end\n"
    "\n"
    "  local ma = ref.draw(ga, x, y, w, h, list_ref, spec)\n"
    "  local mb = door.draw(gb, x, y, w, h, list_new, spec)\n"
    "\n"
    "  if spoil then spoil(gb.ops) end\n"
    "\n"
    "  paint_ops(a, ga.ops, pct)\n"
    "  paint_ops(b, gb.ops, pct)\n"
    "\n"
    "  local same = a:log() == b:log() and a:pixels() == b:pixels()\n"
    "               and ma == mb and math.type(ma) == math.type(mb)\n"
    "\n"
    "  return same, a, b\n"
    "end\n"
    "\n"
    "local function history(mode, n, holes)\n"
    "  local list = {}\n"
    "\n"
    "  for i = 1, n do\n"
    "    local s\n"
    "\n"
    "    if mode == \"mirrored\" then\n"
    "      local kind = rand(4)\n"
    "\n"
    "      if kind == 0 then\n"
    "        s = { rand(3000000000), rand(3000000000) }\n"
    "      elseif kind == 1 then\n"
    "        s = { rand(100000) / 7, rand(100000) / 3 }\n"
    "      elseif kind == 2 then\n"
    "        s = { 0, rand(40) }\n"
    "      else\n"
    "        s = { rand(1 << 20) + 0.5, 0 }\n"
    "      end\n"
    "    elseif rand(5) == 0 then\n"
    "      s = { rand(1000) / 10 }                       -- a single series, a float\n"
    "    else\n"
    "      local k = rand(60)\n"
    "      local u = rand(3) == 0 and rand(90) + rand(10) / 3 or rand(101 - k)\n"
    "\n"
    "      s = { k, u }\n"
    "      if rand(25) == 0 then s[1] = -rand(20) end    -- below the floor\n"
    "      if rand(25) == 0 then s[2] = 150 end          -- past the top\n"
    "    end\n"
    "\n"
    "    if holes and rand(6) == 0 then\n"
    "      if rand(2) == 0 then s[1] = nil else s[2] = nil end\n"
    "    end\n"
    "\n"
    "    list[i] = s\n"
    "  end\n"
    "\n"
    "  return list\n"
    "end\n"
    "\n"
    "local cases, bad = 0, 0\n"
    "local SIZES = { { 4, 4, 380, 120 }, { 10, 12, 300, 60 }, { 0, 0, 420, 220 },\n"
    "                { 20, 30, 120, 20 }, { -6, -3, 200, 40 }, { 33, 9, 77, 6 },\n"
    "                { 5, 5, 61, 9 }, { 2, 150, 400, 66 } }\n"
    "local PCTS = { 100, 125, 150, 175, 200 }\n"
    "\n"
    "for round = 1, 400 do\n"
    "  local mode = ({ \"stacked\", \"mirrored\", nil })[rand(3) + 1]\n"
    "  local columns = ({ 60, 150, 7, 33, 60 })[rand(5) + 1]\n"
    "  local n = rand(columns + 6)                         -- now and then longer than its columns\n"
    "  local size = SIZES[rand(#SIZES) + 1]\n"
    "  local list = history(mode, n, rand(3) == 0)\n"
    "  local spec = { mode = mode, columns = columns }\n"
    "\n"
    "  if rand(4) == 0 then spec.colours = { \"accent\" } end            -- the second one nil\n"
    "  if rand(4) == 0 then spec.ground, spec.radius = 0xff222222, 4 end\n"
    "  if mode == \"mirrored\" and rand(4) == 0 then\n"
    "    spec.most = rand(2) == 0 and rand(1 << 30) or rand(1000) / 3\n"
    "  end\n"
    "\n"
    "  local view = nil\n"
    "\n"
    "  if rand(3) == 0 then\n"
    "    view = { rand(40), rand(30), 100 + rand(300), 20 + rand(150) }\n"
    "  end\n"
    "\n"
    "  local pct = PCTS[rand(#PCTS) + 1]\n"
    "  local same = both(list, copy(list), size[1], size[2], size[3], size[4], spec, view, pct)\n"
    "\n"
    "  cases = cases + 1\n"
    "  if not same then\n"
    "    bad = bad + 1\n"
    "    if bad <= 5 then\n"
    "      print((\"  case %d differs: mode %s, %d of %d columns, box %d %d %d %d, %d%%\"):format(\n"
    "            round, tostring(mode), n, columns, size[1], size[2], size[3], size[4], pct))\n"
    "    end\n"
    "  end\n"
    "end\n"
    "\n"
    "check(bad == 0, (\"ui.history.draw and the kit draw alike: %d cases, %d differ\"):format(cases, bad))\n"
    "\n"
    "--\n"
    "-- **A monitor's own rhythm**: a sample a pace through `push`, drawn every\n"
    "-- third, on one list each way. The ring follows the list through `push`\n"
    "-- alone, so one ring is made for the whole run - not one a draw.\n"
    "--\n"
    "do\n"
    "  local before = made\n"
    "  local lr, ln = {}, {}\n"
    "  local differ = 0\n"
    "\n"
    "  for t = 1, 240 do\n"
    "    local s = { rand(50), rand(50) }\n"
    "\n"
    "    ref.push(lr, s, 60)\n"
    "    door.push(ln, { s[1], s[2] }, 60)\n"
    "\n"
    "    if t % 3 == 0 then\n"
    "      local same = both(lr, ln, 10, 12, 300, 60, { mode = \"stacked\", columns = 60 }, nil, 100)\n"
    "\n"
    "      if not same then differ = differ + 1 end\n"
    "    end\n"
    "\n"
    "    -- Changed behind `push`'s back, once: read into its ring again.\n"
    "    if t == 100 then\n"
    "      table.remove(lr, 1)\n"
    "      table.remove(ln, 1)\n"
    "    end\n"
    "  end\n"
    "\n"
    "  check(differ == 0, (\"a history pushed and drawn as Monitor does draws alike (%d differ)\"):format(differ))\n"
    "  check(made - before == 1, (\"one ring for one history, not one a draw (%d made)\"):format(made - before))\n"
    "end\n"
    "\n"
    "--\n"
    "-- **The controls.** A graph drawn one column off must fail the\n"
    "-- comparison, and so must a column a pixel short - or the checks above\n"
    "-- could not have failed either.\n"
    "--\n"
    "do\n"
    "  local list = history(\"stacked\", 60, false)\n"
    "  local spec = { mode = \"stacked\", columns = 60 }\n"
    "  local same = both(list, copy(list), 10, 12, 300, 60, spec, nil, 100, function(ops)\n"
    "    for _, o in ipairs(ops) do\n"
    "      if o.op == \"series\" then o.upto = o.upto - 1 end\n"
    "    end\n"
    "  end)\n"
    "\n"
    "  check(not same, \"control: a graph drawn one column off is seen to differ\")\n"
    "\n"
    "  list = history(\"mirrored\", 40, false)\n"
    "  same = both(list, copy(list), 10, 12, 300, 60, { mode = \"mirrored\", columns = 60 }, nil, 150,\n"
    "              function(ops)\n"
    "                for _, o in ipairs(ops) do\n"
    "                  if o.op == \"series\" then o.h = o.h - 2 end\n"
    "                end\n"
    "              end)\n"
    "\n"
    "  check(not same, \"control: a mirrored graph two pixels short is seen to differ\")\n"
    "end\n"
    "\n"
    "--\n"
    "-- **A line**, the mode S1 adds: a constant series is one row across, a\n"
    "-- step is joined by a pixel-wide rise, a gap breaks it.\n"
    "--\n"
    "do\n"
    "  local r = SERIES.new{ series = 1, columns = 60 }\n"
    "  local s = surface(W, H)\n"
    "\n"
    "  for i = 1, 30 do r:push({ i <= 20 and 50 or 100 }) end\n"
    "  r:push({ nil })\n"
    "  for _ = 1, 9 do r:push({ 0 }) end\n"
    "\n"
    "  r:draw(s, 0, 0, 304, 104, { mode = \"line\", columns = 40, colours = { 0xff00ff00 } })\n"
    "\n"
    "  -- inner 100, so 50 is level 50 of 0..99: row 102 - 1 - 50 = 51; 100 is row 2; 0 is row 101.\n"
    "  local rows = s:rows_of(0xff00ff00)\n"
    "\n"
    "  check(rows[51] and rows[2] and rows[101], \"line: 50, 100 and 0 at their rows\")\n"
    "  check(s:at(2 + (20 * 300) // 40, 30) == 0xff00ff00, \"line: the rise from 50 to 100 is joined\")\n"
    "  check(s:at(2 + (31 * 300) // 40, 60) ~= 0xff00ff00, \"line: a gap is not joined across\")\n"
    "end\n"
    "\n"
    "print((\"held: %s\"):format(DOOR_FROM))\n"
    "\n";

static void drawing(void)
{
    static const luaL_Reg surface_methods[] = {
        { "fill", l_fill }, { "fill_round", l_fill_round }, { "log", l_log },
        { "pixels", l_pixels }, { "at", l_at }, { "rows_of", l_rows_of },
        { NULL, NULL }
    };
    lua_State *L = luaL_newstate();
    char *door = door_from_ui();

    luaL_openlibs(L);

    luaL_newmetatable(L, SURFACE_MT);
    luaL_newlib(L, surface_methods);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    lua_pushcfunction(L, l_surface);
    lua_setglobal(L, "surface");
    lua_pushcfunction(L, l_check);
    lua_setglobal(L, "check");
    kosmos_series_kit(L);
    lua_setglobal(L, "SERIES");
    lua_pushstring(L, door != NULL ? door : DOOR_COPY);
    lua_setglobal(L, "DOOR");
    lua_pushstring(L, door != NULL ? "user/lib/ui.lua's ui.history"
                                   : "this test's copy of the door (ui.lua is not one yet)");
    lua_setglobal(L, "DOOR_FROM");

    if (luaL_loadbuffer(L, HELD, sizeof HELD - 1, "=held") != LUA_OK
        || lua_pcall(L, 0, 0, 0) != LUA_OK) {
        printf("not ok - the drawing's checks raised: %s\n", lua_tostring(L, -1));
        failures++;
    }

    lua_close(L);
    free(door);
}

int main(void)
{
    rings();
    drawing();

    if (failures) {
        printf("test_series: %d of %d failed\n", failures, checks);
        return 1;
    }

    printf("test_series: %d checks passed\n", checks);
    return 0;
}
