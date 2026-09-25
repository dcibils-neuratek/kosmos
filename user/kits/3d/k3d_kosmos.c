/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * /kits/3d: the 3D Kit, reached from Lua.
 *
 *   local k3 = use("/kits/3d")
 *   local scene = k3.scene()
 *   local id = scene:add{ kind = "box", size = { 1.5, 1.5, 1.5 },
 *                         loc = { -1.75, 0.45, 0.75 }, rot = { 0, 0, 24 },
 *                         colour = 0xc33b2c }
 *   scene:set(id, { hidden = true })
 *
 *   local view = k3.view(1000, 740)
 *   view:look(ex, ey, ez, tx, ty, tz, fov)
 *   local triangles = view:draw(scene, surface, x, y,
 *                               { mode = "solid", selected = id, grid = true })
 *   local what = view:pick(px, py)                -- an id, or nil
 *   local sx, sy = view:project(wx, wy, wz)      -- nil behind the eye
 *   view:line(surface, x, y, ax, ay, az, bx, by, bz, colour, alpha, depth)
 *
 * **An object is its id.** Lua keeps ids, never anything that points into
 * the scene: removing an object moves the ones after it, and a pointer kept
 * across that would be to somebody else - which is what the kit's own test
 * found the first time it kept one.
 *
 * A number out of range is an error rather than a clamp: a sphere of two
 * segments is a bug in whoever asked for it, and the app that shows the
 * field is where a person's typing is held to sense.
 */

#include <math.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "k3d.h"

#define SCENE_MT "kosmos.3d.scene"
#define VIEW_MT  "kosmos.3d.view"

uint32_t *kosmos_surface_pixels(lua_State *L, int index,
                                unsigned *w, unsigned *h, unsigned *pitch);

static struct k3d_scene *check_scene(lua_State *L, int index)
{
    return (struct k3d_scene *)luaL_checkudata(L, index, SCENE_MT);
}

static struct k3d_view *check_view(lua_State *L, int index)
{
    struct k3d_view *v = (struct k3d_view *)luaL_checkudata(L, index, VIEW_MT);

    if (v->depth == NULL) {
        luaL_error(L, "this 3D view has been freed");
    }

    return v;
}

/*--------------------------------------------------------------------------
 * The scene.
 *------------------------------------------------------------------------*/

static int l_scene(lua_State *L)
{
    struct k3d_scene *s = (struct k3d_scene *)lua_newuserdatauv(L, sizeof(*s), 0);

    k3d_scene_init(s);
    luaL_setmetatable(L, SCENE_MT);
    return 1;
}

static int l_scene_gc(lua_State *L)
{
    k3d_scene_free(check_scene(L, 1));
    return 0;
}

/* Three numbers from `{ x, y, z }`, or one number for all three. */
static void three(lua_State *L, int t, const char *key, float out[3])
{
    int k;

    lua_getfield(L, t, key);

    if (lua_isnumber(L, -1)) {
        out[0] = out[1] = out[2] = (float)lua_tonumber(L, -1);
    } else if (lua_istable(L, -1)) {
        for (k = 0; k < 3; k++) {
            lua_rawgeti(L, -1, k + 1);

            if (!lua_isnumber(L, -1)) {
                luaL_error(L, "%s wants three numbers", key);
            }

            out[k] = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
    } else if (!lua_isnil(L, -1)) {
        luaL_error(L, "%s wants three numbers", key);
    }

    lua_pop(L, 1);
}

static bool has(lua_State *L, int t, const char *key)
{
    bool yes;

    lua_getfield(L, t, key);
    yes = !lua_isnil(L, -1);
    lua_pop(L, 1);
    return yes;
}

static float number(lua_State *L, int t, const char *key, float lo, float hi)
{
    lua_Number n;

    lua_getfield(L, t, key);
    n = luaL_checknumber(L, -1);
    lua_pop(L, 1);

    if (!(n >= lo && n <= hi)) {
        luaL_error(L, "%s of %f is outside %f to %f", key, (double)n,
                   (double)lo, (double)hi);
    }

    return (float)n;
}

/*
 * Whatever fields the table has, onto the object. A field that changes the
 * shape marks the triangles stale; one that only moves it does not.
 */
static void apply(lua_State *L, struct k3d_object *o, int t)
{
    if (has(L, t, "size")) {
        three(L, t, "size", o->size);

        if (!(o->size[0] > 0 && o->size[1] > 0 && o->size[2] > 0)) {
            luaL_error(L, "a size is more than nothing");
        }

        o->stale = true;
    }

    if (has(L, t, "radius")) {
        o->radius = number(L, t, "radius", 1e-4f, 1e5f);
        o->stale = true;
    }

    if (has(L, t, "radius2")) {
        o->radius2 = number(L, t, "radius2", 0, 1e5f);   /* a cone's point is 0 */
        o->stale = true;
    }

    if (has(L, t, "subdivisions")) {
        o->subdivisions = (int)number(L, t, "subdivisions", 1, 7);
        o->stale = true;
    }

    if (has(L, t, "depth")) {
        o->depth = number(L, t, "depth", 1e-4f, 1e5f);
        o->stale = true;
    }

    /* A grid's squares may be one across; every round thing needs three. */
    if (has(L, t, "segments")) {
        o->segments = (int)number(L, t, "segments", o->kind == K3D_GRID ? 1 : 3, 256);
        o->stale = true;
    }

    if (has(L, t, "rings")) {
        o->rings = (int)number(L, t, "rings", o->kind == K3D_GRID ? 1 : 2, 256);
        o->stale = true;
    }

    three(L, t, "loc", o->loc);
    three(L, t, "rot", o->rot);
    three(L, t, "scale", o->scale);

    if (has(L, t, "colour")) {
        lua_getfield(L, t, "colour");
        o->colour = (uint32_t)luaL_checkinteger(L, -1) & 0xffffffu;
        lua_pop(L, 1);
    }

    if (has(L, t, "alpha")) {
        o->alpha = number(L, t, "alpha", 0, 1);
    }

    if (has(L, t, "smooth")) {
        lua_getfield(L, t, "smooth");
        o->smooth = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }

    if (has(L, t, "hidden")) {
        lua_getfield(L, t, "hidden");
        o->hidden = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
}

static int l_add(lua_State *L)
{
    /* In `enum k3d_kind`'s order, which is what makes the index the kind. */
    static const char *const kinds[] = { "plane", "box", "sphere", "cylinder",
                                         "ico", "cone", "torus", "grid", NULL };
    struct k3d_scene *s = check_scene(L, 1);
    struct k3d_object made, *o;
    uint32_t id;
    int kind;

    luaL_checktype(L, 2, LUA_TTABLE);
    lua_getfield(L, 2, "kind");
    kind = luaL_checkoption(L, -1, NULL, kinds);
    lua_pop(L, 1);

    /* Made aside and only then added: a field refused halfway leaves no
     * half-made object in the scene with an id nobody was given. */
    k3d_object_defaults(&made, (enum k3d_kind)kind);
    apply(L, &made, 2);

    o = k3d_scene_add(s, (enum k3d_kind)kind);

    if (o == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "out of memory");
        return 2;
    }

    id = o->id;
    *o = made;
    o->id = id;
    lua_pushinteger(L, id);
    return 1;
}

static struct k3d_object *find(lua_State *L, struct k3d_scene *s, int index)
{
    lua_Integer id = luaL_checkinteger(L, index);
    struct k3d_object *o = id > 0 ? k3d_scene_find(s, (uint32_t)id) : NULL;

    if (o == NULL) {
        luaL_error(L, "there is no object %d in this scene", (int)id);
    }

    return o;
}

static int l_set(lua_State *L)
{
    struct k3d_scene *s = check_scene(L, 1);
    struct k3d_object *o = find(L, s, 2);

    luaL_checktype(L, 3, LUA_TTABLE);
    apply(L, o, 3);
    return 0;
}

static int l_remove(lua_State *L)
{
    struct k3d_scene *s = check_scene(L, 1);

    lua_pushboolean(L, k3d_scene_remove(s, (uint32_t)luaL_checkinteger(L, 2)));
    return 1;
}

/* One object's triangles, from its numbers; or every shown object's. */
static int l_triangles(lua_State *L)
{
    struct k3d_scene *s = check_scene(L, 1);
    lua_Integer n = 0;
    uint32_t i;

    if (!lua_isnoneornil(L, 2)) {
        lua_pushinteger(L, k3d_triangles(find(L, s, 2)));
        return 1;
    }

    for (i = 0; i < s->count; i++) {
        if (!s->obj[i].hidden) {
            n += k3d_triangles(&s->obj[i]);
        }
    }

    lua_pushinteger(L, n);
    return 1;
}

/*--------------------------------------------------------------------------
 * The view.
 *------------------------------------------------------------------------*/

static int l_view(lua_State *L)
{
    lua_Integer w = luaL_checkinteger(L, 1), h = luaL_checkinteger(L, 2);
    struct k3d_view *v = (struct k3d_view *)lua_newuserdatauv(L, sizeof(*v), 0);

    memset(v, 0, sizeof(*v));
    luaL_setmetatable(L, VIEW_MT);

    if (w < 1 || h < 1 || w > 8192 || h > 8192 || !k3d_view_init(v, (int)w, (int)h)) {
        lua_pushnil(L);
        lua_pushfstring(L, "no 3D view of %d by %d", (int)w, (int)h);
        return 2;
    }

    return 1;
}

static int l_view_gc(lua_State *L)
{
    struct k3d_view *v = (struct k3d_view *)luaL_checkudata(L, 1, VIEW_MT);

    k3d_view_free(v);
    return 0;
}

static int l_size(lua_State *L)
{
    struct k3d_view *v = check_view(L, 1);

    lua_pushinteger(L, v->w);
    lua_pushinteger(L, v->h);
    return 2;
}

static int l_look(lua_State *L)
{
    struct k3d_view *v = check_view(L, 1);
    float eye[3], at[3], fov;
    int k;

    for (k = 0; k < 3; k++) {
        eye[k] = (float)luaL_checknumber(L, 2 + k);
        at[k] = (float)luaL_checknumber(L, 5 + k);
    }

    fov = (float)luaL_checknumber(L, 8);

    if (!(fov > 0.01f && fov < 3.1f)) {
        return luaL_error(L, "a field of view between 0.01 and 3.1 radians");
    }

    k3d_view_look(v, eye, at, fov);
    return 0;
}

/* The surface at `x, y`, holding the whole view, as a target. */
static struct k3d_target target_at(lua_State *L, struct k3d_view *v,
                                   int index, int xi)
{
    unsigned sw, sh, pitch;
    uint32_t *px = kosmos_surface_pixels(L, index, &sw, &sh, &pitch);
    lua_Integer x = luaL_checkinteger(L, xi), y = luaL_checkinteger(L, xi + 1);
    struct k3d_target t;

    if (px == NULL) {
        luaL_error(L, "that surface has been freed");
    }

    if (x < 0 || y < 0 || x + v->w > (lua_Integer)sw || y + v->h > (lua_Integer)sh) {
        luaL_error(L, "a view of %d by %d does not fit the surface at %d, %d",
                   v->w, v->h, (int)x, (int)y);
    }

    t.pitch = pitch / 4;
    t.px = px + (size_t)y * t.pitch + (size_t)x;
    return t;
}

static uint32_t opt_colour(lua_State *L, int t, const char *key, uint32_t dflt)
{
    uint32_t c = dflt;

    lua_getfield(L, t, key);

    if (!lua_isnil(L, -1)) {
        c = (uint32_t)luaL_checkinteger(L, -1) & 0xffffffu;
    }

    lua_pop(L, 1);
    return c;
}

static int l_draw(lua_State *L)
{
    struct k3d_view *v = check_view(L, 1);
    struct k3d_scene *s = check_scene(L, 2);
    struct k3d_target t = target_at(L, v, 3, 4);
    struct k3d_draw how;
    struct k3d_drawn drawn;

    how.mode = K3D_SOLID;
    how.selected = 0;
    how.grid = true;
    how.sky_top = 0x40444b;
    how.sky_bottom = 0x2d3035;
    how.outline = 0xffa53d;

    if (lua_istable(L, 6)) {
        lua_getfield(L, 6, "mode");

        if (!lua_isnil(L, -1)) {
            static const char *const modes[] = { "solid", "wire", NULL };

            how.mode = (enum k3d_mode)luaL_checkoption(L, -1, NULL, modes);
        }

        lua_pop(L, 1);
        lua_getfield(L, 6, "selected");
        how.selected = lua_isinteger(L, -1) ? (uint32_t)lua_tointeger(L, -1) : 0;
        lua_pop(L, 1);
        lua_getfield(L, 6, "grid");
        how.grid = lua_isnil(L, -1) ? true : lua_toboolean(L, -1);
        lua_pop(L, 1);
        how.sky_top = opt_colour(L, 6, "sky_top", how.sky_top);
        how.sky_bottom = opt_colour(L, 6, "sky_bottom", how.sky_bottom);
        how.outline = opt_colour(L, 6, "outline", how.outline);
    }

    drawn = k3d_draw(v, s, &t, &how);
    lua_pushinteger(L, drawn.triangles);
    lua_pushinteger(L, drawn.objects);
    return 2;
}

static int l_pick(lua_State *L)
{
    struct k3d_view *v = check_view(L, 1);
    uint32_t id = k3d_pick(v, (int)luaL_checkinteger(L, 2),
                           (int)luaL_checkinteger(L, 3));

    if (id == 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, id);
    }

    return 1;
}

static int l_project(lua_State *L)
{
    struct k3d_view *v = check_view(L, 1);
    float p[3] = { (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3),
                   (float)luaL_checknumber(L, 4) };
    float sx, sy, iz;

    if (!k3d_project(v, p, &sx, &sy, &iz)) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushnumber(L, sx);
    lua_pushnumber(L, sy);
    lua_pushnumber(L, 1.0 / iz);
    return 3;
}

/* A segment in the world onto the surface, behind what the last draw put in
 * front of it when `depth` is true. */
static int l_line(lua_State *L)
{
    struct k3d_view *v = check_view(L, 1);
    struct k3d_target t = target_at(L, v, 2, 3);
    float a[3], b[3];
    lua_Integer alpha;
    int k;

    for (k = 0; k < 3; k++) {
        a[k] = (float)luaL_checknumber(L, 5 + k);
        b[k] = (float)luaL_checknumber(L, 8 + k);
    }

    alpha = luaL_optinteger(L, 12, 255);

    if (alpha < 0 || alpha > 255) {
        return luaL_error(L, "an alpha of 0 to 255");
    }

    k3d_line(v, &t, a, b, (uint32_t)luaL_checkinteger(L, 11) & 0xffffffu,
             (unsigned)alpha, lua_toboolean(L, 13));
    return 0;
}

void kosmos_3d_kit(lua_State *L)
{
    static const luaL_Reg scene_methods[] = {
        { "add",       l_add },
        { "set",       l_set },
        { "remove",    l_remove },
        { "triangles", l_triangles },
        { NULL, NULL }
    };
    static const luaL_Reg view_methods[] = {
        { "size",    l_size },
        { "look",    l_look },
        { "draw",    l_draw },
        { "pick",    l_pick },
        { "project", l_project },
        { "line",    l_line },
        { NULL, NULL }
    };
    static const luaL_Reg api[] = {
        { "scene", l_scene },
        { "view",  l_view },
        { NULL, NULL }
    };

    if (luaL_newmetatable(L, SCENE_MT)) {
        luaL_newlib(L, scene_methods);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_scene_gc);
        lua_setfield(L, -2, "__gc");
    }

    lua_pop(L, 1);

    if (luaL_newmetatable(L, VIEW_MT)) {
        luaL_newlib(L, view_methods);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_view_gc);
        lua_setfield(L, -2, "__gc");
    }

    lua_pop(L, 1);
    luaL_newlib(L, api);
}
