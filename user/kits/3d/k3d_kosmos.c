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
 *   local job = k3.render(scene, { w = 640, h = 360, eye = { 7, -6, 4 },
 *                                  target = { 0, 0, 0.7 }, fov = 0.69,
 *                                  passes = 256, world = { ... },
 *                                  lights = { { loc = { -4, -6, 9 }, ... } } })
 *   job:paint(surface, x, y)          -- the tiles that moved on since last
 *   job:passes(), job:rays(), job:workers()
 *   job:stop()                        -- and the workers are waited for
 *
 * **A render is traced by every processor the machine has**, one thread
 * each, started here and waited for here - Lua cannot hand the kernel an
 * entry point, so the kit does. The workers step down to the LOW band as
 * they start, below every window: the machine is the render's only when
 * nothing else wants it, and a window stays as quick to answer as it was.
 * They never allocate, because `malloc` is not under a lock yet
 * (`threads.md` step 7); the render gives them everything before they
 * start, and nothing touches it until they have been waited for.
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
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "kosmos.h"
#include "k3d.h"

#define SCENE_MT  "kosmos.3d.scene"
#define VIEW_MT   "kosmos.3d.view"
#define RENDER_MT "kosmos.3d.render"

#define WORKERS_MAX 64
#define WORKER_BAND 1           /* SCHED_PRIO_LOW, `kernel/sched.h` */

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

/* A colour as a person picks it, 0xRRGGBB in sRGB, as the light it is. */
static void linear(uint32_t c, float out[3])
{
    int k;

    for (k = 0; k < 3; k++) {
        double v = (double)((c >> (16 - 8 * k)) & 0xff) / 255;

        out[k] = (float)(v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4));
    }
}

static bool has(lua_State *L, int t, const char *key)
{
    bool yes;

    lua_getfield(L, t, key);
    yes = !lua_isnil(L, -1);
    lua_pop(L, 1);
    return yes;
}

static float number(lua_State *L, int t, const char *key, float lo, float hi);

/*
 * **A mesh's triangles, as glTF keeps them**: `vertices` three little-endian
 * floats a point, `triangles` three indices a face of `index_bytes` each -
 * 1, 2 or 4 - and `yup` for points in glTF's Y-up rather than this kit's
 * Z-up. Strings rather than tables, because a mesh is a buffer and a
 * table of a hundred thousand numbers is exactly what C over Lua values is
 * slowest at. Last in `apply`, so a field refused before it leaves no mesh.
 */
static void triangles(lua_State *L, struct k3d_object *o, int t)
{
    size_t vb, tb, i;
    const char *v, *tr;
    lua_Integer ib = 4;
    float angle = 30, *pos;
    uint32_t *idx, nverts, ntris;
    bool yup, ok;

    if (o->kind != K3D_MESH) {
        luaL_error(L, "only a mesh is given its triangles");
    }

    lua_getfield(L, t, "vertices");
    v = luaL_checklstring(L, -1, &vb);
    lua_getfield(L, t, "triangles");
    tr = luaL_checklstring(L, -1, &tb);
    lua_getfield(L, t, "index_bytes");
    ib = luaL_optinteger(L, -1, 4);
    lua_getfield(L, t, "yup");
    yup = lua_toboolean(L, -1);
    lua_pop(L, 2);

    if (has(L, t, "smooth_angle")) {
        angle = number(L, t, "smooth_angle", 0, 180);
    }

    if ((ib != 1 && ib != 2 && ib != 4) || vb == 0 || vb % 12 != 0
        || tb == 0 || tb % (size_t)(ib * 3) != 0 || vb / 12 > (1u << 24)) {
        luaL_error(L, "a mesh is three floats a point and three indices a face");
    }

    nverts = (uint32_t)(vb / 12);
    ntris = (uint32_t)(tb / (size_t)(ib * 3));
    pos = malloc(vb);
    idx = malloc((size_t)ntris * 3 * sizeof(uint32_t));

    if (pos == NULL || idx == NULL) {
        free(pos);
        free(idx);
        luaL_error(L, "no memory for a mesh of %d faces", (int)ntris);
    }

    memcpy(pos, v, vb);

    /* glTF's Y up is this kit's Z: (x, y, z) there is (x, -z, y) here. */
    if (yup) {
        for (i = 0; i < nverts; i++) {
            float y = pos[i * 3 + 1];

            pos[i * 3 + 1] = -pos[i * 3 + 2];
            pos[i * 3 + 2] = y;
        }
    }

    for (i = 0; i < (size_t)ntris * 3; i++) {
        const unsigned char *b = (const unsigned char *)tr + i * (size_t)ib;

        idx[i] = ib == 1 ? b[0]
               : ib == 2 ? (uint32_t)b[0] | (uint32_t)b[1] << 8
               : (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16
                 | (uint32_t)b[3] << 24;
    }

    lua_pop(L, 2);
    ok = k3d_mesh_set(o, pos, nverts, idx, ntris, angle);
    free(pos);
    free(idx);

    if (!ok) {
        luaL_error(L, "a mesh whose faces name points it does not have");
    }
}

/*
 * **A texture, from `{ pattern = "brick", colour2 = 0xb8b0a4, scale = 13,
 * bump = 0.004, ... }`** (`k3d_texture.c`). Blender's numbers and ranges,
 * each held to its range: a mortar of the whole brick, or a bump a metre
 * deep, is a mistake in whoever asked. What is not given keeps what the
 * texture had, and `false` takes a texture off.
 */
static void texture(lua_State *L, int t, struct k3d_texture *out)
{
    static const char *const patterns[] = { "plain", "checker", "brick", "shingles",
                                            "noise", "wood", "marble", NULL };

    if (!lua_toboolean(L, t)) {
        out->pattern = K3D_PLAIN;
        return;
    }

    luaL_checktype(L, t, LUA_TTABLE);
    lua_getfield(L, t, "pattern");
    out->pattern = (enum k3d_pattern)luaL_checkoption(L, -1, NULL, patterns);
    lua_pop(L, 1);

    if (has(L, t, "colour2")) {
        lua_getfield(L, t, "colour2");
        linear((uint32_t)luaL_checkinteger(L, -1), out->colour2);
        lua_pop(L, 1);
    }

    if (has(L, t, "scale"))      { out->scale = number(L, t, "scale", 0.01f, 1000); }
    if (has(L, t, "detail"))     { out->detail = number(L, t, "detail", 0, 8); }
    if (has(L, t, "distortion")) { out->distortion = number(L, t, "distortion", 0, 20); }
    if (has(L, t, "bump"))       { out->bump = number(L, t, "bump", 0, 0.2f); }
    if (has(L, t, "mortar"))     { out->mortar = number(L, t, "mortar", 0, 0.9f); }
    if (has(L, t, "ratio"))      { out->ratio = number(L, t, "ratio", 0.1f, 10); }
    if (has(L, t, "offset"))     { out->offset = number(L, t, "offset", 0, 1); }
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

    if (has(L, t, "faceted")) {
        lua_getfield(L, t, "faceted");
        o->faceted = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }

    if (has(L, t, "mat")) {
        int m;

        lua_getfield(L, t, "mat");
        m = lua_gettop(L);
        luaL_checktype(L, m, LUA_TTABLE);

        if (has(L, m, "base")) {
            lua_getfield(L, m, "base");
            linear((uint32_t)luaL_checkinteger(L, -1), o->mat.base);
            lua_pop(L, 1);
        }

        if (has(L, m, "metallic")) { o->mat.metallic = number(L, m, "metallic", 0, 1); }
        if (has(L, m, "rough"))    { o->mat.rough = number(L, m, "rough", 0, 1); }
        if (has(L, m, "trans"))    { o->mat.trans = number(L, m, "trans", 0, 1); }
        if (has(L, m, "ior"))      { o->mat.ior = number(L, m, "ior", 1, 4); }
        if (has(L, m, "emit"))     { o->mat.emit = number(L, m, "emit", 0, 1000); }

        if (has(L, m, "texture")) {
            lua_getfield(L, m, "texture");
            texture(L, lua_gettop(L), &o->mat.tex);
            lua_pop(L, 1);
        }

        lua_pop(L, 1);
    }

    if (has(L, t, "vertices") || has(L, t, "triangles")) {
        triangles(L, o, t);
    }
}

static int l_add(lua_State *L)
{
    /* In `enum k3d_kind`'s order, which is what makes the index the kind. */
    static const char *const kinds[] = { "plane", "box", "sphere", "cylinder",
                                         "ico", "cone", "torus", "grid", "mesh",
                                         NULL };
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
/*
 * `scene:world_triangles(id [, "yup"])` -> points, triangles: an object's
 * triangles where they are in the world, for exporting. Z up as this scene
 * is, or glTF's and OBJ's Y up when asked. A mirrored object - a scale
 * below nought an odd number of times - has its triangles turned round, so
 * they still face outwards once the mirror is baked in.
 */
static int l_world_triangles(lua_State *L)
{
    struct k3d_scene *s = check_scene(L, 1);
    struct k3d_object *o = find(L, s, 2);
    bool yup = lua_isstring(L, 3) && strcmp(lua_tostring(L, 3), "yup") == 0;
    int below = (o->scale[0] < 0) + (o->scale[1] < 0) + (o->scale[2] < 0);
    bool flip = (below & 1) != 0;
    float m[12];
    luaL_Buffer b;
    uint32_t i;

    if (o->kind != K3D_MESH && (o->stale || o->mesh.pos == NULL)) {
        if (!k3d_mesh_build(o)) return luaL_error(L, "no memory for its triangles");
        o->stale = false;
    }

    k3d_object_matrix(o, m);
    luaL_buffinit(L, &b);

    for (i = 0; i < o->mesh.nverts; i++) {
        const float *p = &o->mesh.pos[i * 3];
        float w[3];
        int k;

        for (k = 0; k < 3; k++) {
            w[k] = m[k * 4] * p[0] + m[k * 4 + 1] * p[1] + m[k * 4 + 2] * p[2] + m[k * 4 + 3];
        }

        if (yup) {
            float y = w[1];

            w[1] = w[2];
            w[2] = -y;
        }

        luaL_addlstring(&b, (const char *)w, 12);
    }

    luaL_pushresult(&b);
    luaL_buffinit(L, &b);

    for (i = 0; i < o->mesh.ntris; i++) {
        uint32_t t[3] = { o->mesh.tri[i * 3], o->mesh.tri[i * 3 + 1], o->mesh.tri[i * 3 + 2] };

        if (flip) {
            uint32_t x = t[1];

            t[1] = t[2];
            t[2] = x;
        }

        luaL_addlstring(&b, (const char *)t, 12);
    }

    luaL_pushresult(&b);
    return 2;
}

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

/*--------------------------------------------------------------------------
 * A render.
 *------------------------------------------------------------------------*/

struct job {
    struct k3d_render *r;
    long worker[WORKERS_MAX];
    int workers;
    int w, h;
};

static void yield_now(void)
{
    kosmos_yield();
}

static void worker(unsigned long arg)
{
    struct k3d_render *r = (struct k3d_render *)(uintptr_t)arg;
    uint32_t tile, pass;

    (void)kosmos_sched_set(SCHED_SET_MY_BAND, WORKER_BAND);

    while (k3d_render_job(r, &tile, &pass, yield_now)) {
        k3d_render_tile(r, tile, pass);
    }

    kosmos_thread_exit(0);
}

/* Stopped, its workers waited for, and freed: nothing runs in it after. */
static void job_end(struct job *j)
{
    int i;

    if (j->r == NULL) {
        return;
    }

    k3d_render_stop(j->r);

    for (i = 0; i < j->workers; i++) {
        (void)kosmos_thread_wait((unsigned long)j->worker[i]);
    }

    j->workers = 0;
    k3d_render_free(j->r);
    j->r = NULL;
}

static struct job *check_job(lua_State *L, int index)
{
    return (struct job *)luaL_checkudata(L, index, RENDER_MT);
}

static float opt_number(lua_State *L, int t, const char *key, float dflt, float lo, float hi)
{
    return has(L, t, key) ? number(L, t, key, lo, hi) : dflt;
}

static void opt_linear(lua_State *L, int t, const char *key, uint32_t dflt, float out[3])
{
    linear(opt_colour(L, t, key, dflt), out);
}

/* The lamps, from `{ { loc = { x, y, z }, radius, colour, power }, ... }`. */
static int lamps(lua_State *L, int t, struct k3d_light *out, int most)
{
    int n = 0, i, count;

    lua_getfield(L, t, "lights");

    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }

    luaL_checktype(L, -1, LUA_TTABLE);
    count = (int)lua_rawlen(L, -1);

    if (count > most) {
        luaL_error(L, "a render takes at most %d lamps", most);
    }

    for (i = 1; i <= count; i++) {
        struct k3d_light *l = &out[n++];
        int e;

        lua_rawgeti(L, -1, i);
        e = lua_gettop(L);
        luaL_checktype(L, e, LUA_TTABLE);
        three(L, e, "loc", l->pos);
        l->radius = opt_number(L, e, "radius", 0.1f, 0, 1e4f);
        l->power = opt_number(L, e, "power", 1000, 0, 1e7f);
        opt_linear(L, e, "colour", 0xffffff, l->colour);
        lua_pop(L, 1);
    }

    lua_pop(L, 1);
    return n;
}

static int l_render(lua_State *L)
{
    struct k3d_scene *s = check_scene(L, 1);
    struct k3d_light lights[16];
    struct k3d_render_setup how;
    struct sysinfo info;
    struct job *j;
    int want, i;

    luaL_checktype(L, 2, LUA_TTABLE);
    memset(&how, 0, sizeof(how));
    how.w = (int)number(L, 2, "w", 1, 8192);
    how.h = (int)number(L, 2, "h", 1, 8192);
    three(L, 2, "eye", how.eye);
    three(L, 2, "target", how.target);
    how.fov = number(L, 2, "fov", 0.01f, 3.1f);
    how.passes = (uint32_t)opt_number(L, 2, "passes", 256, 1, 65536);
    how.bounces = (int)opt_number(L, 2, "bounces", 6, 1, 32);

    lua_getfield(L, 2, "preview");
    how.preview = lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 2, "world");

    if (lua_istable(L, -1)) {
        int w = lua_gettop(L);

        opt_linear(L, w, "zenith", 0x6d90c6, how.world.zenith);
        opt_linear(L, w, "horizon", 0xdfe6ef, how.world.horizon);
        how.world.strength = opt_number(L, w, "strength", 1, 0, 1000);
    }

    lua_pop(L, 1);
    how.nlights = lamps(L, 2, lights, (int)(sizeof(lights) / sizeof(lights[0])));
    how.lights = lights;

    /* Every processor that runs threads, unless asked for fewer. */
    want = kosmos_sysinfo(&info) == 0 && info.cpus > 0 ? (int)info.cpus : 1;
    want = (int)opt_number(L, 2, "threads", (float)want, 1, WORKERS_MAX);

    j = (struct job *)lua_newuserdatauv(L, sizeof(*j), 0);
    memset(j, 0, sizeof(*j));
    luaL_setmetatable(L, RENDER_MT);
    j->w = how.w;
    j->h = how.h;
    j->r = k3d_render_new(s, &how);

    if (j->r == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "out of memory for a render");
        return 2;
    }

    for (i = 0; i < want; i++) {
        long index = kosmos_thread_start(worker, (unsigned long)(uintptr_t)j->r);

        if (index < 0) {
            break;                      /* as many as the machine would give */
        }

        j->worker[j->workers++] = index;
    }

    if (j->workers == 0) {
        job_end(j);
        lua_pushnil(L);
        lua_pushstring(L, "no thread to render with");
        return 2;
    }

    return 1;
}

/*
 * `k3.unbase64(text)` - the bytes a glTF file's `data:` URI carries, which
 * is how a scene file keeps its meshes. A loop over bytes, so here rather
 * than in Lua; anything that is not base64 is refused rather than skipped.
 */
static int l_unbase64(lua_State *L)
{
    size_t len, i;
    const unsigned char *in = (const unsigned char *)luaL_checklstring(L, 1, &len);
    luaL_Buffer b;
    uint32_t acc = 0;
    int bits = 0;

    luaL_buffinit(L, &b);

    for (i = 0; i < len; i++) {
        unsigned c = in[i], v;

        if (c >= 'A' && c <= 'Z') {
            v = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            v = c - 'a' + 26;
        } else if (c >= '0' && c <= '9') {
            v = c - '0' + 52;
        } else if (c == '+') {
            v = 62;
        } else if (c == '/') {
            v = 63;
        } else if (c == '=') {
            break;
        } else {
            return luaL_error(L, "not base64 at byte %d", (int)i);
        }

        acc = (acc << 6) | v;
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            luaL_addchar(&b, (char)((acc >> bits) & 0xff));
        }
    }

    luaL_pushresult(&b);
    return 1;
}

/*
 * `k3.base64(bytes)` - the other way, for saving: a mesh's points and
 * triangles as the text of a `data:` URI, padded with `=` as the
 * standard says, so any program's reader takes it.
 */
static int l_base64(lua_State *L)
{
    static const char digits[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len, i;
    const unsigned char *in = (const unsigned char *)luaL_checklstring(L, 1, &len);
    luaL_Buffer b;

    luaL_buffinit(L, &b);

    for (i = 0; i + 2 < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];

        luaL_addchar(&b, digits[v >> 18]);
        luaL_addchar(&b, digits[(v >> 12) & 63]);
        luaL_addchar(&b, digits[(v >> 6) & 63]);
        luaL_addchar(&b, digits[v & 63]);
    }

    if (len - i == 1) {
        uint32_t v = (uint32_t)in[i] << 16;

        luaL_addchar(&b, digits[v >> 18]);
        luaL_addchar(&b, digits[(v >> 12) & 63]);
        luaL_addstring(&b, "==");
    } else if (len - i == 2) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8;

        luaL_addchar(&b, digits[v >> 18]);
        luaL_addchar(&b, digits[(v >> 12) & 63]);
        luaL_addchar(&b, digits[(v >> 6) & 63]);
        luaL_addchar(&b, '=');
    }

    luaL_pushresult(&b);
    return 1;
}

/*
 * `k3.bounds(points)` -> min x, y, z, max x, y, z of a mesh's points, three
 * little-endian floats each, as a file keeps them. glTF requires them of a
 * mesh's positions, and they are a loop over every point.
 */
static int l_bounds(lua_State *L)
{
    size_t len, i;
    const char *p = luaL_checklstring(L, 1, &len);
    float lo[3], hi[3];
    int k;

    if (len < 12 || len % 12 != 0) {
        return luaL_error(L, "points are three floats each, and there are none");
    }

    for (i = 0; i < len; i += 12) {
        float v[3];

        memcpy(v, p + i, sizeof(v));

        for (k = 0; k < 3; k++) {
            if (i == 0 || v[k] < lo[k]) lo[k] = v[k];
            if (i == 0 || v[k] > hi[k]) hi[k] = v[k];
        }
    }

    for (k = 0; k < 3; k++) lua_pushnumber(L, (lua_Number)lo[k]);
    for (k = 0; k < 3; k++) lua_pushnumber(L, (lua_Number)hi[k]);

    return 6;
}

/*
 * `k3.gather(bytes, offset, count, stride, width)` - `count` pieces of
 * `width` bytes, `stride` apart from `offset`, packed together: a mesh's
 * points out of a buffer where each sits among the rest of its vertex, as
 * many programs write them. nil and why for pieces the bytes do not hold.
 */
static int l_gather(lua_State *L)
{
    size_t len;
    const char *in = luaL_checklstring(L, 1, &len);
    lua_Integer offset = luaL_checkinteger(L, 2), count = luaL_checkinteger(L, 3);
    lua_Integer stride = luaL_checkinteger(L, 4), width = luaL_checkinteger(L, 5);
    luaL_Buffer b;
    lua_Integer i;

    if (offset < 0 || count < 0 || width <= 0 || stride < width || count > (1 << 26)
        || (count > 0 && (uint64_t)offset + (uint64_t)(count - 1) * (uint64_t)stride
                         + (uint64_t)width > len)) {
        lua_pushnil(L);
        lua_pushstring(L, "points past the end of their buffer");
        return 2;
    }

    luaL_buffinit(L, &b);

    for (i = 0; i < count; i++) {
        luaL_addlstring(&b, in + offset + i * stride, (size_t)width);
    }

    luaL_pushresult(&b);
    return 1;
}

/* `k3.sequence(n)` - 0, 1, 2 ... n - 1 as little-endian 32-bit indices:
 * the triangles of a mesh that lists none, every three points one. */
static int l_sequence(lua_State *L)
{
    lua_Integer n = luaL_checkinteger(L, 1), i;
    luaL_Buffer b;

    if (n < 0 || n > (1 << 26)) return luaL_error(L, "%d is not a number of points", (int)n);

    luaL_buffinit(L, &b);

    for (i = 0; i < n; i++) {
        char le[4] = { (char)(i & 0xff), (char)((i >> 8) & 0xff), (char)((i >> 16) & 0xff),
                       (char)((i >> 24) & 0xff) };

        luaL_addlstring(&b, le, 4);
    }

    luaL_pushresult(&b);
    return 1;
}

/* A soup's points and triangles as two strings, three floats a point and
 * three little-endian uint32 a triangle - the shape a mesh thing keeps. */
static void push_soup(lua_State *L, const struct k3d_soup *s)
{
    lua_pushlstring(L, (const char *)s->pos, (size_t)s->npos * 12);
    lua_pushlstring(L, (const char *)s->tri, (size_t)s->ntri * 12);
}

/*
 * `k3.read_stl(bytes)` -> points, triangles, or nil and why. The points
 * come out as a mesh here keeps them, glTF's Y up - STL is Z up, as this
 * scene is, so (x, y, z) is written (x, z, -y) for the kit to take back.
 */
static int l_read_stl(lua_State *L)
{
    size_t len;
    const unsigned char *in = (const unsigned char *)luaL_checklstring(L, 1, &len);
    struct k3d_soup s;
    const char *why = k3d_stl_read(in, len, &s);
    uint32_t i;

    if (why) {
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    }

    for (i = 0; i < s.npos; i++) {
        float y = s.pos[i * 3 + 1];

        s.pos[i * 3 + 1] = s.pos[i * 3 + 2];
        s.pos[i * 3 + 2] = -y;
    }

    push_soup(L, &s);
    k3d_soup_free(&s);
    return 2;
}

/* `k3.read_obj(text)` -> { mtllib, parts = { { name, material, points,
 * triangles }, ... } }, or nil and why. OBJ is Y up already. */
static int l_read_obj(lua_State *L)
{
    size_t len, i;
    const char *in = luaL_checklstring(L, 1, &len);
    struct k3d_obj o;
    const char *why = k3d_obj_read(in, len, &o);

    if (why) {
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    }

    lua_createtable(L, 0, 2);
    lua_pushstring(L, o.mtllib);
    lua_setfield(L, -2, "mtllib");
    lua_createtable(L, (int)o.nparts, 0);

    for (i = 0; i < o.nparts; i++) {
        lua_createtable(L, 0, 4);
        lua_pushstring(L, o.parts[i].name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, o.parts[i].material);
        lua_setfield(L, -2, "material");
        push_soup(L, &o.parts[i].soup);
        lua_setfield(L, -3, "triangles");
        lua_setfield(L, -2, "points");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }

    lua_setfield(L, -2, "parts");
    k3d_obj_free(&o);
    return 1;
}

static void set_numbers(lua_State *L, const char *key, const float *v, int n)
{
    int i;

    lua_createtable(L, n, 0);

    for (i = 0; i < n; i++) {
        lua_pushnumber(L, v[i]);
        lua_rawseti(L, -2, i + 1);
    }

    lua_setfield(L, -2, key);
}

/*
 * `k3.read_fbx(bytes)` -> { parts = { { name, material, hidden, matrix,
 * points, triangles }, ... }, materials = { { name, base, emit, metallic,
 * rough, trans, ior }, ... }, lamps, cameras }, or nil and why. A part's
 * points are Y up and in metres in its own space, and `matrix` - sixteen
 * numbers, column-major, as glTF's - puts them in the world; `material`
 * counts from one into `materials`, or is absent. Colours are linear.
 */
static int l_read_fbx(lua_State *L)
{
    size_t len, i;
    const unsigned char *in = (const unsigned char *)luaL_checklstring(L, 1, &len);
    struct k3d_fbx f;
    const char *why = k3d_fbx_read(in, len, &f);

    if (why) {
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    }

    lua_createtable(L, 0, 4);
    lua_createtable(L, (int)f.nparts, 0);

    for (i = 0; i < f.nparts; i++) {
        const struct k3d_fbx_part *p = &f.parts[i];
        int k;

        lua_createtable(L, 0, 6);
        lua_pushstring(L, p->name);
        lua_setfield(L, -2, "name");

        if (p->material < f.nmaterials) {
            lua_pushinteger(L, (lua_Integer)p->material + 1);
            lua_setfield(L, -2, "material");
        }

        lua_pushboolean(L, p->hidden);
        lua_setfield(L, -2, "hidden");
        lua_createtable(L, 16, 0);

        for (k = 0; k < 16; k++) {
            lua_pushnumber(L, p->matrix[k]);
            lua_rawseti(L, -2, k + 1);
        }

        lua_setfield(L, -2, "matrix");
        push_soup(L, &p->soup);
        lua_setfield(L, -3, "triangles");
        lua_setfield(L, -2, "points");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }

    lua_setfield(L, -2, "parts");
    lua_createtable(L, (int)f.nmaterials, 0);

    for (i = 0; i < f.nmaterials; i++) {
        const struct k3d_fbx_material *m = &f.materials[i];

        lua_createtable(L, 0, 7);
        lua_pushstring(L, m->name);
        lua_setfield(L, -2, "name");
        set_numbers(L, "base", m->base, 3);
        set_numbers(L, "emit", m->emit, 3);
        lua_pushnumber(L, m->metallic);
        lua_setfield(L, -2, "metallic");
        lua_pushnumber(L, m->rough);
        lua_setfield(L, -2, "rough");
        lua_pushnumber(L, m->trans);
        lua_setfield(L, -2, "trans");
        lua_pushnumber(L, m->ior);
        lua_setfield(L, -2, "ior");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }

    lua_setfield(L, -2, "materials");
    lua_pushinteger(L, f.lamps);
    lua_setfield(L, -2, "lamps");
    lua_pushinteger(L, f.cameras);
    lua_setfield(L, -2, "cameras");
    k3d_fbx_free(&f);
    return 1;
}

/* A soup out of a Lua table { points = ..., triangles = ... }; the strings
 * stay the table's, so the soup is only borrowed while the table lives. */
static void soup_of(lua_State *L, int t, struct k3d_soup *s)
{
    size_t np, nt;
    const char *p, *tr;

    lua_getfield(L, t, "points");
    lua_getfield(L, t, "triangles");
    p = luaL_checklstring(L, -2, &np);
    tr = luaL_checklstring(L, -1, &nt);
    lua_pop(L, 2);

    if (np % 12 || nt % 12) luaL_error(L, "points and triangles are twelve bytes each");

    s->pos = (float *)(uintptr_t)p;
    s->tri = (uint32_t *)(uintptr_t)tr;
    s->npos = (uint32_t)(np / 12);
    s->ntri = (uint32_t)(nt / 12);

    {
        uint32_t i;

        for (i = 0; i < s->ntri * 3; i++) {
            uint32_t v;

            memcpy(&v, tr + i * 4, 4);

            if (v >= s->npos) luaL_error(L, "a triangle naming a point there is not");
        }
    }
}

/* `k3.write_stl(parts, scale)` -> the bytes of a binary STL of every part. */
static int l_write_stl(lua_State *L)
{
    lua_Integer n, i;
    struct k3d_soup *soups;
    float scale = (float)luaL_optnumber(L, 2, 1);
    unsigned char *out;
    size_t len;

    luaL_checktype(L, 1, LUA_TTABLE);
    n = (lua_Integer)lua_rawlen(L, 1);
    soups = lua_newuserdatauv(L, (size_t)(n > 0 ? n : 1) * sizeof(*soups), 0);

    for (i = 0; i < n; i++) {
        lua_rawgeti(L, 1, i + 1);
        luaL_checktype(L, -1, LUA_TTABLE);
        soup_of(L, lua_gettop(L), &soups[i]);
        lua_pop(L, 1);
    }

    out = k3d_stl_write(soups, (size_t)n, scale, &len);

    if (!out) return luaL_error(L, "no memory for the STL");

    lua_pushlstring(L, (const char *)out, len);
    free(out);
    return 1;
}

/* `k3.write_obj(parts, mtllib)` -> the text of an OBJ of every part, each
 * `{ name, material, points, triangles }`. */
static int l_write_obj(lua_State *L)
{
    lua_Integer n, i;
    struct k3d_obj_part *parts;
    const char *mtllib = luaL_optstring(L, 2, NULL);
    char *out;
    size_t len;

    luaL_checktype(L, 1, LUA_TTABLE);
    n = (lua_Integer)lua_rawlen(L, 1);
    parts = lua_newuserdatauv(L, (size_t)(n > 0 ? n : 1) * sizeof(*parts), 0);
    memset(parts, 0, (size_t)(n > 0 ? n : 1) * sizeof(*parts));

    for (i = 0; i < n; i++) {
        lua_rawgeti(L, 1, i + 1);
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_getfield(L, -1, "name");
        lua_getfield(L, -2, "material");
        snprintf(parts[i].name, sizeof(parts[i].name), "%s", luaL_optstring(L, -2, "Object"));
        snprintf(parts[i].material, sizeof(parts[i].material), "%s", luaL_optstring(L, -1, ""));
        lua_pop(L, 2);
        soup_of(L, lua_gettop(L), &parts[i].soup);
        lua_pop(L, 1);
    }

    out = k3d_obj_write(parts, (size_t)n, mtllib, &len);

    if (!out) return luaL_error(L, "no memory for the OBJ");

    lua_pushlstring(L, out, len);
    free(out);
    return 1;
}

static int l_job_gc(lua_State *L)
{
    job_end(check_job(L, 1));
    return 0;
}

static struct job *live_job(lua_State *L)
{
    struct job *j = check_job(L, 1);

    if (j->r == NULL) {
        luaL_error(L, "this render has been stopped");
    }

    return j;
}

static int l_job_paint(lua_State *L)
{
    struct job *j = live_job(L);
    unsigned sw, sh, pitch;
    uint32_t *px = kosmos_surface_pixels(L, 2, &sw, &sh, &pitch);
    lua_Integer x = luaL_checkinteger(L, 3), y = luaL_checkinteger(L, 4);

    if (px == NULL) {
        return luaL_error(L, "that surface has been freed");
    }

    if (x < 0 || y < 0 || x + j->w > (lua_Integer)sw || y + j->h > (lua_Integer)sh) {
        return luaL_error(L, "a render of %d by %d does not fit the surface at %d, %d",
                          j->w, j->h, (int)x, (int)y);
    }

    lua_pushinteger(L, k3d_render_paint(j->r, px + (size_t)y * (pitch / 4) + (size_t)x,
                                        pitch / 4, lua_toboolean(L, 5)));
    return 1;
}

static int l_job_passes(lua_State *L)
{
    lua_pushinteger(L, k3d_render_passes(live_job(L)->r));
    return 1;
}

static int l_job_rays(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)k3d_render_rays(live_job(L)->r));
    return 1;
}

static int l_job_workers(lua_State *L)
{
    lua_pushinteger(L, live_job(L)->workers);
    return 1;
}

static int l_job_stop(lua_State *L)
{
    job_end(check_job(L, 1));
    return 0;
}

void kosmos_3d_kit(lua_State *L)
{
    static const luaL_Reg scene_methods[] = {
        { "add",       l_add },
        { "set",       l_set },
        { "remove",    l_remove },
        { "triangles", l_triangles },
        { "world_triangles", l_world_triangles },
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
    static const luaL_Reg job_methods[] = {
        { "paint",   l_job_paint },
        { "passes",  l_job_passes },
        { "rays",    l_job_rays },
        { "workers", l_job_workers },
        { "stop",    l_job_stop },
        { NULL, NULL }
    };
    static const luaL_Reg api[] = {
        { "scene",  l_scene },
        { "view",   l_view },
        { "render", l_render },
        { "unbase64", l_unbase64 },
        { "base64", l_base64 },
        { "bounds", l_bounds },
        { "gather", l_gather },
        { "sequence", l_sequence },
        { "read_stl", l_read_stl },
        { "read_obj", l_read_obj },
        { "read_fbx", l_read_fbx },
        { "write_stl", l_write_stl },
        { "write_obj", l_write_obj },
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

    if (luaL_newmetatable(L, RENDER_MT)) {
        luaL_newlib(L, job_methods);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_job_gc);
        lua_setfield(L, -2, "__gc");
    }

    lua_pop(L, 1);
    luaL_newlib(L, api);
}
