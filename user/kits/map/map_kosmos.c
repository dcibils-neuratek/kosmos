/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Map Kit** - `use("/Kosmos/Kits/map")` (`docs/maps.md`) - a map's
 * regions, tiles and drawing, for Maps and for anything else that wants a
 * map: a photo's place, a run's track.
 *
 *   local map = use("/Kosmos/Kits/map")
 *   local region, why = map.open(bytes)       a PMTiles archive's bytes, kept
 *   region:info()                             { min_zoom, max_zoom, west, south,
 *                                               east, north, lon, lat, zoom }
 *   local tile, why = region:tile(z, x, y)    decoded once, kept by the caller
 *   local tile, why = map.decode(bytes)       a tile from its bytes (gzipped or not)
 *   tile:draw(surface, x, y, size, zoom, style, cx0, cy0, cx1, cy1)
 *                                             its top left at x, y, `size` points
 *                                             a side; nothing outside the clip
 *   tile:labels()                             { { name, layer, class, rank, x, y,
 *                                               angle }, ... }: x, y from 0 to 1
 *                                             across the tile
 *   tile:count()                              features, parts, points
 *   local style = map.style{ background = c, rules = { { layer = "water",
 *       fill = c }, { layer = "transportation", classes = "primary,trunk",
 *       line = c, width = { 12, 1, 16, 6 }, minzoom = 12 }, ... } }
 *   map.project(lon, lat)                     x, y across the world, 0 to 1
 *   map.unproject(x, y)                       lon, lat
 *
 * **Lua decides, C draws** (`CLAUDE.md`): which tiles and where is the
 * caller's; decoding and drawing a tile, a few thousand points every frame
 * of a drag, are here (`mvt.c`, `mapdraw.c`), through gfx's paths.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "kits/compress/gzip.h"
#include "kits/gfx/gfx_draw.h"

#include "mapdraw.h"
#include "mvt.h"
#include "pmtiles.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define REGION "kosmos.map.region"
#define TILE   "kosmos.map.tile"
#define STYLE  "kosmos.map.style"

struct region { struct pmtiles a; int open; };
struct tile   { struct mvt_tile t; struct map_matches m; int live; };

static int push_tile(lua_State *L, const uint8_t *bytes, size_t n);

static const char *const LAYER_NAMES[L_COUNT] = {
    [L_OTHER] = "other", [L_WATER] = "water", [L_WATERWAY] = "waterway",
    [L_LANDCOVER] = "landcover", [L_LANDUSE] = "landuse", [L_PARK] = "park",
    [L_BUILDING] = "building", [L_TRANSPORTATION] = "transportation",
    [L_TRANSPORTATION_NAME] = "transportation_name", [L_PLACE] = "place",
    [L_WATER_NAME] = "water_name", [L_POI] = "poi", [L_BOUNDARY] = "boundary",
    [L_AEROWAY] = "aeroway",
};

/* ---- regions ---- */

static int l_open(lua_State *L)
{
    size_t n;
    const char *bytes = luaL_checklstring(L, 1, &n);
    struct region *r = lua_newuserdatauv(L, sizeof *r, 1);
    struct gunzip_work *work = kosmos_inflater();
    int result;

    memset(r, 0, sizeof *r);
    luaL_setmetatable(L, REGION);

    /* The bytes are kept with the region: the archive points into them. */
    lua_pushvalue(L, 1);
    lua_setiuservalue(L, -2, 1);

    if (work == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "no memory for the inflater");
        return 2;
    }

    result = pmt_open(&r->a, (const uint8_t *)bytes, n, work);

    if (result != PMT_OK) {
        pmt_close(&r->a);
        lua_pushnil(L);
        lua_pushstring(L, pmt_said(result));
        return 2;
    }

    if (r->a.tile_type != 1) {
        pmt_close(&r->a);
        lua_pushnil(L);
        lua_pushstring(L, "its tiles are not vector tiles");
        return 2;
    }

    r->open = 1;
    return 1;
}

static struct region *check_region(lua_State *L)
{
    struct region *r = luaL_checkudata(L, 1, REGION);

    if (!r->open) luaL_error(L, "this region was closed");

    return r;
}

static int l_region_gc(lua_State *L)
{
    struct region *r = luaL_checkudata(L, 1, REGION);

    if (r->open) pmt_close(&r->a);

    r->open = 0;
    return 0;
}

static void field_n(lua_State *L, const char *k, lua_Number v)
{
    lua_pushnumber(L, v);
    lua_setfield(L, -2, k);
}

static int l_info(lua_State *L)
{
    struct region *r = check_region(L);

    lua_newtable(L);
    field_n(L, "min_zoom", r->a.min_zoom);
    field_n(L, "max_zoom", r->a.max_zoom);
    field_n(L, "west", r->a.min_lon_e7 / 1e7);
    field_n(L, "south", r->a.min_lat_e7 / 1e7);
    field_n(L, "east", r->a.max_lon_e7 / 1e7);
    field_n(L, "north", r->a.max_lat_e7 / 1e7);
    field_n(L, "lon", r->a.centre_lon_e7 / 1e7);
    field_n(L, "lat", r->a.centre_lat_e7 / 1e7);
    field_n(L, "zoom", r->a.centre_zoom);
    field_n(L, "tiles", (lua_Number)r->a.nroot);
    return 1;
}

/* A tile's inflated bytes, gathered into a buffer kept between tiles. */
struct gather { uint8_t *bytes; size_t n, cap; int short_of_memory; };
static struct gather inflated;

static int gather_put(void *user, const uint8_t *b, size_t n)
{
    struct gather *g = user;

    if (g->n + n > g->cap) {
        size_t cap = g->cap ? g->cap : 65536;
        uint8_t *grown;

        while (cap < g->n + n) cap *= 2;

        if ((grown = realloc(g->bytes, cap)) == NULL) {
            g->short_of_memory = 1;
            return 0;
        }

        g->bytes = grown, g->cap = cap;
    }

    memcpy(g->bytes + g->n, b, n);
    g->n += n;
    return 1;
}

static int l_tile(lua_State *L)
{
    struct region *r = check_region(L);
    lua_Integer z = luaL_checkinteger(L, 2);
    lua_Integer x = luaL_checkinteger(L, 3);
    lua_Integer y = luaL_checkinteger(L, 4);
    const uint8_t *bytes;
    size_t n, got;
    int result;

    if (z < 0 || z > 30 || x < 0 || y < 0) {
        lua_pushnil(L);
        lua_pushstring(L, pmt_said(PMT_NOT_FOUND));
        return 2;
    }

    result = pmt_find(&r->a, (unsigned)z, (uint32_t)x, (uint32_t)y, &bytes, &n);

    if (result != PMT_OK) {
        lua_pushnil(L);
        lua_pushstring(L, pmt_said(result));
        return 2;
    }

    if (r->a.tile_gzip) {
        inflated.n = 0, inflated.short_of_memory = 0;

        if (kosmos_gunzip(bytes, n, kosmos_inflater(), gather_put, &inflated, &got)
            != GUNZIP_WHOLE) {
            lua_pushnil(L);
            lua_pushstring(L, inflated.short_of_memory ? "no memory for the tile"
                                                       : "the tile would not inflate");
            return 2;
        }

        bytes = inflated.bytes, n = inflated.n;
    }

    return push_tile(L, bytes, n);
}

/* A decoded tile, pushed; or nil and why. */
static int push_tile(lua_State *L, const uint8_t *bytes, size_t n)
{
    struct tile *t = lua_newuserdatauv(L, sizeof *t, 0);
    int result;

    memset(t, 0, sizeof *t);
    luaL_setmetatable(L, TILE);

    if ((result = mvt_decode(&t->t, bytes, n)) != MVT_OK) {
        lua_pushnil(L);
        lua_pushstring(L, result == MVT_NO_MEMORY ? "no memory for the tile"
                                                  : "the tile is not a vector tile");
        return 2;
    }

    t->live = 1;
    return 1;
}

/*
 * `map.decode(bytes)` - a tile from its bytes, gzipped or not: what the map's
 * `tiles` server keeps in `/Home/Cache/Maps` (`docs/maps.md` M6d), read by
 * whoever draws it. nil and why for bytes that are not a vector tile.
 */
static int l_decode(lua_State *L)
{
    size_t n, got;
    const uint8_t *bytes = (const uint8_t *)luaL_checklstring(L, 1, &n);

    if (n >= 2 && bytes[0] == 0x1f && bytes[1] == 0x8b) {
        inflated.n = 0, inflated.short_of_memory = 0;

        if (kosmos_gunzip(bytes, n, kosmos_inflater(), gather_put, &inflated, &got)
            != GUNZIP_WHOLE) {
            lua_pushnil(L);
            lua_pushstring(L, "the tile would not inflate");
            return 2;
        }

        bytes = inflated.bytes, n = inflated.n;
    }

    return push_tile(L, bytes, n);
}

/* ---- tiles ---- */

static struct tile *check_tile(lua_State *L, int index)
{
    struct tile *t = luaL_checkudata(L, index, TILE);

    if (!t->live) luaL_error(L, "this tile was freed");

    return t;
}

static int l_tile_gc(lua_State *L)
{
    struct tile *t = luaL_checkudata(L, 1, TILE);

    if (t->live) {
        mvt_free(&t->t);
        map_matches_free(&t->m);
    }

    t->live = 0;
    return 0;
}

static int l_count(lua_State *L)
{
    struct tile *t = check_tile(L, 1);

    lua_pushinteger(L, (lua_Integer)t->t.nfeatures);
    lua_pushinteger(L, (lua_Integer)t->t.nparts);
    lua_pushinteger(L, (lua_Integer)t->t.npoints);
    return 3;
}

struct style { struct map_style s; int live; };

static struct gfx_path draw_path;

static int l_draw(lua_State *L)
{
    struct tile *t = check_tile(L, 1);
    unsigned w, h, pitch;
    uint32_t *pixels = kosmos_surface_pixels(L, 2, &w, &h, &pitch);
    float ox = (float)luaL_checknumber(L, 3), oy = (float)luaL_checknumber(L, 4);
    float size = (float)luaL_checknumber(L, 5), zoom = (float)luaL_checknumber(L, 6);
    struct style *st = luaL_checkudata(L, 7, STYLE);
    long cx0 = (long)luaL_optinteger(L, 8, 0), cy0 = (long)luaL_optinteger(L, 9, 0);
    long cx1 = (long)luaL_optinteger(L, 10, (lua_Integer)w);
    long cy1 = (long)luaL_optinteger(L, 11, (lua_Integer)h);

    lua_pushinteger(L, map_draw(&t->t, &t->m, &st->s, &draw_path, pixels, pitch, (long)w,
                                (long)h, ox, oy, size, zoom, cx0, cy0, cx1, cy1));
    return 1;
}

/*
 * Where a feature's name goes: a point's own place; along a line, the
 * middle of its longest part and the way it runs there; a polygon's middle.
 */
static void label_place(const struct mvt_tile *t, const struct mvt_feature *f,
                        double *x, double *y, double *angle)
{
    const struct mvt_part *best = &t->parts[f->part0];
    double best_len = -1;

    *angle = 0;

    if (f->type == MVT_POINT || f->nparts == 0) {
        *x = t->xy[2 * best->first], *y = t->xy[2 * best->first + 1];
        return;
    }

    if (f->type == MVT_POLYGON) {
        double sx = 0, sy = 0;

        for (uint32_t i = 0; i < best->count; i++) {
            sx += t->xy[2 * (best->first + i)];
            sy += t->xy[2 * (best->first + i) + 1];
        }

        *x = sx / best->count, *y = sy / best->count;
        return;
    }

    for (uint32_t pi = 0; pi < f->nparts; pi++) {
        const struct mvt_part *pt = &t->parts[f->part0 + pi];
        double len = 0;

        for (uint32_t i = 0; i + 1 < pt->count; i++) {
            const float *a = &t->xy[2 * (pt->first + i)];

            len += hypot(a[2] - a[0], a[3] - a[1]);
        }

        if (len > best_len) best_len = len, best = pt;
    }

    {
        double half = best_len / 2, run = 0;

        *x = t->xy[2 * best->first], *y = t->xy[2 * best->first + 1];

        for (uint32_t i = 0; i + 1 < best->count; i++) {
            const float *a = &t->xy[2 * (best->first + i)];
            double seg = hypot(a[2] - a[0], a[3] - a[1]);

            if (run + seg >= half && seg > 0) {
                double f2 = (half - run) / seg;

                *x = a[0] + (a[2] - a[0]) * f2;
                *y = a[1] + (a[3] - a[1]) * f2;
                *angle = atan2(a[3] - a[1], a[2] - a[0]);

                /* Never upside down: a name reads left to right. */
                if (*angle > M_PI / 2) *angle -= M_PI;
                if (*angle < -M_PI / 2) *angle += M_PI;
                return;
            }

            run += seg;
        }
    }
}

static int l_labels(lua_State *L)
{
    struct tile *t = check_tile(L, 1);
    double ext = (double)(t->t.extent ? t->t.extent : 4096);
    int n = 0;

    lua_newtable(L);

    for (size_t i = 0; i < t->t.nfeatures; i++) {
        const struct mvt_feature *f = &t->t.features[i];
        double x, y, angle;

        if (f->name == NULL || f->name[0] == '\0') continue;

        /* What a map writes: places, points of interest, roads and water by
         * name, and named parks. A street's own line draws it; its name is
         * `transportation_name`'s. */
        if (f->layer != L_PLACE && f->layer != L_POI && f->layer != L_TRANSPORTATION_NAME
            && f->layer != L_WATER_NAME && f->layer != L_PARK) continue;

        label_place(&t->t, f, &x, &y, &angle);

        if (x < 0 || y < 0 || x >= ext || y >= ext) continue;

        lua_createtable(L, 0, 7);
        lua_pushstring(L, f->name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, LAYER_NAMES[f->layer]);
        lua_setfield(L, -2, "layer");
        lua_pushstring(L, f->klass);
        lua_setfield(L, -2, "class");
        lua_pushinteger(L, f->rank);
        lua_setfield(L, -2, "rank");
        field_n(L, "x", x / ext);
        field_n(L, "y", y / ext);
        field_n(L, "angle", f->type == MVT_LINE ? angle : 0);
        lua_rawseti(L, -2, ++n);
    }

    return 1;
}

/* ---- styles ---- */

static uint32_t style_versions;

static int l_style_gc(lua_State *L)
{
    struct style *st = luaL_checkudata(L, 1, STYLE);

    if (st->live) free(st->s.rules);

    st->live = 0;
    return 0;
}

static int layer_named(const char *name)
{
    for (int i = 0; i < L_COUNT; i++) {
        if (LAYER_NAMES[i] && strcmp(LAYER_NAMES[i], name) == 0) return i;
    }

    return -1;
}

static int l_style(lua_State *L)
{
    struct style *st;
    size_t n;

    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "rules");
    luaL_checktype(L, -1, LUA_TTABLE);
    n = lua_rawlen(L, -1);

    st = lua_newuserdatauv(L, sizeof *st, 0);
    memset(st, 0, sizeof *st);
    luaL_setmetatable(L, STYLE);

    st->s.rules = calloc(n ? n : 1, sizeof *st->s.rules);

    if (st->s.rules == NULL) return luaL_error(L, "no memory for a style of %d rules", (int)n);

    st->live = 1;
    st->s.version = ++style_versions;

    lua_getfield(L, 1, "background");
    st->s.background = (uint32_t)luaL_optinteger(L, -1, 0) & 0xffffffu;
    lua_pop(L, 1);

    for (size_t i = 0; i < n; i++) {
        struct map_rule *r = &st->s.rules[st->s.nrules];
        const char *layer, *classes;
        int kind;

        lua_rawgeti(L, -2, (lua_Integer)i + 1);
        luaL_checktype(L, -1, LUA_TTABLE);

        lua_getfield(L, -1, "layer");
        layer = lua_tostring(L, -1);
        kind = layer ? layer_named(layer) : -1;
        lua_pop(L, 1);

        if (kind < 0) {
            return luaL_error(L, "rule %d: no layer called %s", (int)i + 1,
                              layer ? layer : "(none)");
        }

        r->layer = (uint8_t)kind;

        lua_getfield(L, -1, "classes");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_getfield(L, -1, "class");
        }
        classes = lua_tostring(L, -1);
        if (classes && strlen(classes) >= MAP_CLASSES) {
            return luaL_error(L, "rule %d: its classes are longer than %d", (int)i + 1, MAP_CLASSES - 1);
        }
        strcpy(r->classes, classes ? classes : "");
        lua_pop(L, 1);

        lua_getfield(L, -1, "fill");
        if (!lua_isnil(L, -1)) {
            r->kind = MAP_FILL;
            r->colour = (uint32_t)lua_tointeger(L, -1) & 0xffffffu;
        }
        lua_pop(L, 1);

        lua_getfield(L, -1, "line");
        if (!lua_isnil(L, -1)) {
            r->kind = MAP_LINE;
            r->colour = (uint32_t)lua_tointeger(L, -1) & 0xffffffu;
        }
        lua_pop(L, 1);

        if (r->kind == 0) return luaL_error(L, "rule %d: neither a fill nor a line", (int)i + 1);

        lua_getfield(L, -1, "minzoom");
        r->minzoom = (float)luaL_optnumber(L, -1, 0);
        lua_pop(L, 1);
        lua_getfield(L, -1, "maxzoom");
        r->maxzoom = (float)luaL_optnumber(L, -1, 99);
        lua_pop(L, 1);

        lua_getfield(L, -1, "width");
        if (lua_istable(L, -1)) {
            size_t k = lua_rawlen(L, -1) / 2;

            if (k > MAP_STOPS) k = MAP_STOPS;

            for (size_t s = 0; s < 2 * k; s++) {
                lua_rawgeti(L, -1, (lua_Integer)s + 1);
                r->stops[s] = (float)lua_tonumber(L, -1);
                lua_pop(L, 1);
            }

            r->nstops = (int)k;
        } else if (lua_isnumber(L, -1)) {
            r->stops[0] = 0, r->stops[1] = (float)lua_tonumber(L, -1);
            r->nstops = 1;
        }
        lua_pop(L, 1);

        lua_pop(L, 1);
        st->s.nrules++;
    }

    return 1;
}

/* ---- positions ---- */

static int l_project(lua_State *L)
{
    double lon = luaL_checknumber(L, 1), lat = luaL_checknumber(L, 2);
    double r;

    if (lat > 85.05112878) lat = 85.05112878;
    if (lat < -85.05112878) lat = -85.05112878;

    r = lat * M_PI / 180.0;
    lua_pushnumber(L, (lon + 180.0) / 360.0);
    lua_pushnumber(L, (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0);
    return 2;
}

static int l_unproject(lua_State *L)
{
    double x = luaL_checknumber(L, 1), y = luaL_checknumber(L, 2);

    lua_pushnumber(L, x * 360.0 - 180.0);
    lua_pushnumber(L, atan(sinh(M_PI * (1.0 - 2.0 * y))) * 180.0 / M_PI);
    return 2;
}

void kosmos_map_kit(lua_State *L)
{
    static const luaL_Reg region[] = {
        { "info", l_info }, { "tile", l_tile }, { "__gc", l_region_gc }, { NULL, NULL }
    };
    static const luaL_Reg tile[] = {
        { "draw", l_draw }, { "labels", l_labels }, { "count", l_count },
        { "__gc", l_tile_gc }, { NULL, NULL }
    };
    static const luaL_Reg style[] = { { "__gc", l_style_gc }, { NULL, NULL } };
    static const struct { const char *name; const luaL_Reg *methods; } kinds[] = {
        { REGION, region }, { TILE, tile }, { STYLE, style },
    };

    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        if (luaL_newmetatable(L, kinds[i].name)) {
            lua_pushvalue(L, -1);
            lua_setfield(L, -2, "__index");
            luaL_setfuncs(L, kinds[i].methods, 0);
        }

        lua_pop(L, 1);
    }

    lua_newtable(L);
    lua_pushcfunction(L, l_open);
    lua_setfield(L, -2, "open");
    lua_pushcfunction(L, l_style);
    lua_setfield(L, -2, "style");
    lua_pushcfunction(L, l_decode);
    lua_setfield(L, -2, "decode");
    lua_pushcfunction(L, l_project);
    lua_setfield(L, -2, "project");
    lua_pushcfunction(L, l_unproject);
    lua_setfield(L, -2, "unproject");
}
