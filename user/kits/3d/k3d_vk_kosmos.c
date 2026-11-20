/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The 3D Kit on Vulkan, for Lua** (`docs/flight.md` F4c): the door an
 * application draws a terrain through - Flight first, and Cafesa3D's Solid
 * view after it. It lives in an image of its own, `k3dvk.elf`, with Mesa
 * linked in, because the system's image does not carry a Vulkan driver;
 * an application says `-- kosmos: image k3dvk.elf` and reaches it as
 * `use("k3dvk.elf")`.
 *
 *     local k = use("k3dvk.elf")
 *     local w = k.window("Flight", 640, 400)
 *     local g = k.terrain{ lon = .., lat = .., root = 12, deepest = 16,
 *                          radius = 2, split = 2, most = 400, elevation = 14 }
 *     for _, t in ipairs(g:wanted(eye)) do ... end   -- { kind, z, x, y }
 *     g:elevation(z, x, y, gfx.png(bytes))          -- a terrarium tile
 *     g:imagery(z, x, y, gfx.jpeg(bytes))           -- the picture over it
 *     local said = w:draw(g, view)                  -- a frame, presented
 *
 * A tile's pixels stay in C: Lua hands over the surface gfx decoded and
 * this reads it where it lies (`kosmos_surface_pixels`). A chunk is built -
 * its mesh from the heights of the elevation tile over it, its imagery
 * from the picture at its own zoom - the first frame both have come, and
 * kept; a chunk whose tiles have not come is not drawn. F4d moves the
 * decoding and the building off the frame's thread, and gives the chunks
 * a frame leaves behind back.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"

#include "kits/kitenv.h"
#include "kits/window/kosmos_window.h"
#include "kits/gfx/gfx_draw.h"
#include "kits/map/elevation.h"
#include "kits/map/mercator.h"
#include "k3d_vk.h"

#define WINDOW  "k3dvk.window"
#define TERRAIN "k3dvk.terrain"

/* What Mesa or LLVM says - an abort's last words among it - goes to the
 * console rather than nowhere (`runtime/posix/fd.c`), as `vulkantest.c`
 * hands it. */
void posix_console(long capability);
long posix_console_capability(void);

static void hand_console(lua_State *L)
{
    if (posix_console_capability() < 0) {
        posix_console(kit_capability(L, "/Devices/console"));
    }
}

static double seconds(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* ------------------------------------------------------------ a terrain */

/* An elevation tile's heights, or a chunk built: kept in a list that grows. */
struct heights {
    struct k3d_tile at;
    struct map_heights h;
};

struct picture {
    struct k3d_tile at;
    uint32_t *pixels;               /* 0xAARRGGBB, packed */
    unsigned width, height;
};

struct chunk {
    struct k3d_tile at;
    struct k3d_vk_ground ground;
};

struct window;

struct terrain {
    struct k3d_place place;
    struct k3d_choice choice;
    unsigned most, elevation;       /* chunks at most; the elevation's deepest zoom */
    struct heights *heights;
    unsigned nheights, heights_room;
    struct picture *pictures;
    unsigned npictures, pictures_room;
    struct chunk *chunks;
    unsigned nchunks, chunks_room;
    struct k3d_tile *chosen;        /* `most` of them */
    /* The window the chunks were built in - held as the terrain's user
     * value, so it is not collected first - and which opening of it. */
    struct window *owner;
    unsigned serial;
};

static int same(struct k3d_tile a, struct k3d_tile b)
{
    return a.z == b.z && a.x == b.x && a.y == b.y;
}

/* The tile at zoom `z` over `t`: `t` itself or an ancestor. */
static struct k3d_tile above(struct k3d_tile t, unsigned z)
{
    if (t.z <= z) {
        return t;
    }
    unsigned up = t.z - z;
    return (struct k3d_tile){ t.x >> up, t.y >> up, (uint8_t)z };
}

static struct map_heights *heights_of(struct terrain *g, struct k3d_tile t)
{
    for (unsigned i = 0; i < g->nheights; i++) {
        if (same(g->heights[i].at, t)) {
            return &g->heights[i].h;
        }
    }
    return NULL;
}

static struct picture *picture_of(struct terrain *g, struct k3d_tile t)
{
    for (unsigned i = 0; i < g->npictures; i++) {
        if (same(g->pictures[i].at, t)) {
            return &g->pictures[i];
        }
    }
    return NULL;
}

static struct chunk *chunk_of(struct terrain *g, struct k3d_tile t)
{
    for (unsigned i = 0; i < g->nchunks; i++) {
        if (same(g->chunks[i].at, t)) {
            return &g->chunks[i];
        }
    }
    return NULL;
}

/* Room for one more in a list, doubling: 0 when memory is out. */
static int room(void **list, unsigned *room_of, unsigned count, size_t size)
{
    if (count < *room_of) {
        return 1;
    }
    unsigned more = *room_of ? *room_of * 2 : 16;
    void *bigger = realloc(*list, (size_t)more * size);

    if (bigger == NULL) {
        return 0;
    }
    *list = bigger;
    *room_of = more;
    return 1;
}

static struct terrain *terrain_check(lua_State *L, int index)
{
    return luaL_checkudata(L, index, TERRAIN);
}

static double field(lua_State *L, int table, const char *name)
{
    double value;

    lua_getfield(L, table, name);
    value = luaL_checknumber(L, -1);
    lua_pop(L, 1);
    return value;
}

static void terrain_free_chunks(struct terrain *g);

static int l_terrain(lua_State *L)
{
    struct terrain *g;

    luaL_checktype(L, 1, LUA_TTABLE);
    g = lua_newuserdatauv(L, sizeof(*g), 1);
    memset(g, 0, sizeof(*g));
    luaL_setmetatable(L, TERRAIN);
    k3d_place_at(&g->place, field(L, 1, "lon"), field(L, 1, "lat"));
    g->choice.root = (unsigned)field(L, 1, "root");
    g->choice.deepest = (unsigned)field(L, 1, "deepest");
    g->choice.radius = (unsigned)field(L, 1, "radius");
    g->choice.split = field(L, 1, "split");
    g->most = (unsigned)field(L, 1, "most");
    g->elevation = (unsigned)field(L, 1, "elevation");
    luaL_argcheck(L, g->choice.root <= g->choice.deepest && g->choice.deepest <= 24, 1,
                  "zooms from root to deepest, at most 24");
    luaL_argcheck(L, g->most >= 1 && g->most <= 65536, 1, "most from 1");
    luaL_argcheck(L, g->elevation >= g->choice.root, 1, "the elevation's zoom from the root's");
    g->chosen = calloc(g->most, sizeof(*g->chosen));
    if (g->chosen == NULL) {
        return luaL_error(L, "out of memory for a terrain");
    }
    return 1;
}

static int l_terrain_gc(lua_State *L)
{
    struct terrain *g = terrain_check(L, 1);

    terrain_free_chunks(g);
    for (unsigned i = 0; i < g->nheights; i++) {
        map_heights_free(&g->heights[i].h);
    }
    for (unsigned i = 0; i < g->npictures; i++) {
        free(g->pictures[i].pixels);
    }
    free(g->heights);
    free(g->pictures);
    free(g->chunks);
    free(g->chosen);
    memset(g, 0, sizeof(*g));
    return 0;
}

static void eye_of(lua_State *L, int index, double eye[3])
{
    luaL_checktype(L, index, LUA_TTABLE);
    for (int i = 0; i < 3; i++) {
        lua_rawgeti(L, index, i + 1);
        eye[i] = luaL_checknumber(L, -1);
        lua_pop(L, 1);
    }
}

/* g:wanted{x, y, z} -> the tiles the chunks for that eye need and have not
 * been given: { "e", z, x, y } and { "i", z, x, y }, each once. */
static int l_terrain_wanted(lua_State *L)
{
    struct terrain *g = terrain_check(L, 1);
    double eye[3];
    unsigned n, listed = 0;

    eye_of(L, 2, eye);
    n = k3d_terrain_choose(&g->place, &g->choice, eye, g->chosen, g->most);
    lua_newtable(L);
    for (unsigned k = 0; k < n; k++) {
        struct k3d_tile want[2] = { above(g->chosen[k], g->elevation), g->chosen[k] };

        for (int kind = 0; kind < 2; kind++) {
            int have = kind == 0 ? heights_of(g, want[0]) != NULL
                                 : picture_of(g, want[1]) != NULL;
            int earlier = 0;

            /* Several chunks share one elevation tile: listed once. */
            for (unsigned j = 0; j < k && !earlier && kind == 0; j++) {
                earlier = same(above(g->chosen[j], g->elevation), want[0]);
            }
            if (have || earlier) {
                continue;
            }
            lua_createtable(L, 4, 0);
            lua_pushstring(L, kind == 0 ? "e" : "i");
            lua_rawseti(L, -2, 1);
            lua_pushinteger(L, want[kind].z);
            lua_rawseti(L, -2, 2);
            lua_pushinteger(L, want[kind].x);
            lua_rawseti(L, -2, 3);
            lua_pushinteger(L, want[kind].y);
            lua_rawseti(L, -2, 4);
            lua_rawseti(L, -2, (lua_Integer)++listed);
        }
    }
    return 1;
}

static struct k3d_tile tile_args(lua_State *L, int first)
{
    lua_Integer z = luaL_checkinteger(L, first), x = luaL_checkinteger(L, first + 1);
    lua_Integer y = luaL_checkinteger(L, first + 2);

    luaL_argcheck(L, z >= 0 && z <= 30, first, "a zoom from 0 to 30");
    luaL_argcheck(L, x >= 0 && x < ((lua_Integer)1 << z), first + 1, "x inside the world");
    luaL_argcheck(L, y >= 0 && y < ((lua_Integer)1 << z), first + 2, "y inside the world");
    return (struct k3d_tile){ (uint32_t)x, (uint32_t)y, (uint8_t)z };
}

/* g:elevation(z, x, y, surface): a terrarium tile's heights, kept. */
static int l_terrain_elevation(lua_State *L)
{
    struct terrain *g = terrain_check(L, 1);
    struct k3d_tile t = tile_args(L, 2);
    unsigned w, h, pitch;
    const uint32_t *px = kosmos_surface_pixels(L, 5, &w, &h, &pitch);

    if (heights_of(g, t) != NULL) {
        return 0;
    }
    if (!room((void **)&g->heights, &g->heights_room, g->nheights, sizeof(*g->heights))) {
        return luaL_error(L, "out of memory for heights");
    }
    struct heights *e = &g->heights[g->nheights];
    memset(e, 0, sizeof(*e));
    e->at = t;
    if (!map_heights_terrarium(&e->h, px, w, h, pitch)) {
        return luaL_error(L, "an elevation tile is at least 2 by 2, and needs memory");
    }
    g->nheights++;
    return 0;
}

/* g:imagery(z, x, y, surface): the picture over a tile, kept. */
static int l_terrain_imagery(lua_State *L)
{
    struct terrain *g = terrain_check(L, 1);
    struct k3d_tile t = tile_args(L, 2);
    unsigned w, h, pitch;
    const uint32_t *px = kosmos_surface_pixels(L, 5, &w, &h, &pitch);

    if (picture_of(g, t) != NULL) {
        return 0;
    }
    luaL_argcheck(L, w >= 1 && h >= 1, 5, "a picture of a pixel at least");
    if (!room((void **)&g->pictures, &g->pictures_room, g->npictures, sizeof(*g->pictures))) {
        return luaL_error(L, "out of memory for imagery");
    }
    struct picture *p = &g->pictures[g->npictures];
    p->at = t;
    p->width = w;
    p->height = h;
    p->pixels = malloc((size_t)w * h * 4);
    if (p->pixels == NULL) {
        return luaL_error(L, "out of memory for imagery");
    }
    for (unsigned y = 0; y < h; y++) {
        memcpy(p->pixels + (size_t)y * w, (const uint8_t *)px + (size_t)y * pitch, (size_t)w * 4);
    }
    g->npictures++;
    return 0;
}

/* The heights under a place, from the elevation tile over it at the
 * terrain's elevation zoom - a chunk asks only inside its own. */
struct sampler {
    const struct map_heights *h;
    struct k3d_tile at;
};

static float sampled(void *context, double mx, double my)
{
    const struct sampler *s = context;
    double n = ldexp(1.0, s->at.z);

    return map_heights_sample(s->h, mx * n - s->at.x, my * n - s->at.y);
}

/* ------------------------------------------------------------- a window */

struct window {
    unsigned serial;                /* 0 once closed */
    struct kvk v;
    struct kw_window *window;
    struct kvk_swapchain chain;
    struct k3d_vk renderer;
    struct k3d_terrain_vertex *corners;
    uint32_t *indices;
    struct k3d_vk_ground *drawn;    /* the frame's, `drawn_room` of them */
    unsigned drawn_room;
    char said[160];
};

/* A window's chunks are freed while it is open; once it is closed its
 * device has gone and taken them. */
static void terrain_free_chunks(struct terrain *g)
{
    struct window *w = g->owner;

    if (w != NULL && w->serial != 0 && w->serial == g->serial) {
        for (unsigned i = 0; i < g->nchunks; i++) {
            k3d_vk_ground_free(&w->renderer, &g->chunks[i].ground);
        }
    }
    g->nchunks = 0;
}

static struct window *window_check(lua_State *L, int index)
{
    struct window *w = luaL_checkudata(L, index, WINDOW);

    if (w->window == NULL) {
        luaL_error(L, "the window is closed");
    }
    return w;
}

static void window_close(struct window *w)
{
    struct kvk *v = &w->v;

    if (v->device != VK_NULL_HANDLE) {
        v->vkDeviceWaitIdle(v->device);
    }
    if (w->renderer.v != NULL) {
        k3d_vk_close(&w->renderer);
    }
    kvk_swapchain_close(v, &w->chain);
    kvk_close(v);
    if (w->window != NULL) {
        kw_close(w->window);
    }
    free(w->corners);
    free(w->indices);
    free(w->drawn);
    memset(w, 0, sizeof(*w));
}

/* k.window(title, width, height) -> a window drawn by Vulkan, or nil and
 * why. */
static int l_window(lua_State *L)
{
    const char *title = luaL_checkstring(L, 1);
    lua_Integer width = luaL_checkinteger(L, 2), height = luaL_checkinteger(L, 3);
    struct window *w;

    luaL_argcheck(L, width >= 16 && width <= 8192, 2, "a width from 16");
    luaL_argcheck(L, height >= 16 && height <= 8192, 3, "a height from 16");
    hand_console(L);
    static unsigned opened;

    w = lua_newuserdatauv(L, sizeof(*w), 0);
    memset(w, 0, sizeof(*w));
    luaL_setmetatable(L, WINDOW);
    w->serial = ++opened ? opened : ++opened;

    const char *why = NULL;
    w->corners = malloc(sizeof(*w->corners) * K3D_CHUNK_VERTICES);
    w->indices = malloc(sizeof(*w->indices) * K3D_CHUNK_INDICES);
    if (w->corners == NULL || w->indices == NULL) {
        why = "memory";
    } else if (!kvk_open(&w->v, "k3dvk", 1)) {
        why = w->v.why;
    } else if ((w->window = kw_open(title, (unsigned)width, (unsigned)height, 0)) == NULL) {
        why = kw_why();
    } else if (!kvk_swapchain_open(&w->v, &w->chain, w->window) ||
               !k3d_vk_open(&w->renderer, &w->v, w->chain.format, w->chain.extent.width,
                            w->chain.extent.height)) {
        why = w->v.why;
    }
    if (why != NULL) {
        char copy[160];

        snprintf(copy, sizeof(copy), "%s", why);
        window_close(w);
        lua_pushnil(L);
        lua_pushstring(L, copy);
        return 2;
    }
    snprintf(w->said, sizeof(w->said), "%s %ux%u", w->v.device_name, w->chain.extent.width,
             w->chain.extent.height);
    return 1;
}

static int l_window_gc(lua_State *L)
{
    struct window *w = luaL_checkudata(L, 1, WINDOW);

    if (w->window != NULL) {
        window_close(w);
    }
    return 0;
}

static int l_window_close(lua_State *L)
{
    return l_window_gc(L);
}

/* w:device() -> "llvmpipe/1.4 320x200": the device and the picture's size. */
static int l_window_device(lua_State *L)
{
    lua_pushstring(L, window_check(L, 1)->said);
    return 1;
}

/* w:closed(wait) -> true once the window was asked to close, waiting up to
 * `wait` seconds for it. */
static int l_window_closed(lua_State *L)
{
    struct window *w = window_check(L, 1);
    double until = seconds() + luaL_optnumber(L, 2, 0.0);
    struct kw_event e;

    do {
        while (kw_poll(w->window, &e, 10)) {
            if (e.type == KW_CLOSE) {
                window_close(w);
                lua_pushboolean(L, 1);
                return 1;
            }
        }
    } while (seconds() < until);
    lua_pushboolean(L, 0);
    return 1;
}

/* The view's fields, as `vulkantest`'s scene has them. */
static void numbers(lua_State *L, int table, const char *name, float *out, int n)
{
    lua_getfield(L, table, name);
    if (n == 1) {
        out[0] = (float)luaL_checknumber(L, -1);
    } else {
        luaL_checktype(L, -1, LUA_TTABLE);
        for (int i = 0; i < n; i++) {
            lua_rawgeti(L, -1, i + 1);
            out[i] = (float)luaL_checknumber(L, -1);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

/* Its chunk built, from its tiles, the first time both have come: 1 when
 * it can be drawn. */
static int chunk_ready(struct window *w, struct terrain *g, struct k3d_tile t,
                       struct k3d_vk_ground **out)
{
    struct chunk *c = chunk_of(g, t);

    if (c == NULL) {
        struct k3d_tile e = above(t, g->elevation);
        struct map_heights *h = heights_of(g, e);
        struct picture *p = picture_of(g, t);

        if (h == NULL || p == NULL ||
            !room((void **)&g->chunks, &g->chunks_room, g->nchunks, sizeof(*g->chunks))) {
            return 0;
        }
        struct sampler s = { h, e };

        k3d_terrain_chunk(&g->place, t, sampled, &s, 20.0f, w->corners, w->indices, NULL, NULL);
        c = &g->chunks[g->nchunks];
        c->at = t;
        if (!k3d_vk_ground(&w->renderer, &c->ground, w->corners, K3D_CHUNK_VERTICES, w->indices,
                           K3D_CHUNK_INDICES, p->pixels, p->width, p->height,
                           VK_FORMAT_B8G8R8A8_UNORM)) {
            return 0;
        }
        g->nchunks++;
    }
    *out = &c->ground;
    return 1;
}

/* w:draw(terrain, view) -> "drawn=N of M ms=T chunks=z/x/y,...", a frame
 * drawn and presented; nil and why when it could not be. */
static int l_window_draw(lua_State *L)
{
    struct window *w = window_check(L, 1);
    struct terrain *g = terrain_check(L, 2);
    struct kvk *v = &w->v;
    struct kvk_swapchain *c = &w->chain;
    struct k3d_vk_view view;
    double eye[3], started = seconds();
    unsigned n, drawn = 0;
    int index;
    luaL_Buffer out;

    luaL_checktype(L, 3, LUA_TTABLE);
    memset(&view, 0, sizeof(view));
    numbers(L, 3, "eye", view.eye, 3);
    numbers(L, 3, "target", view.target, 3);
    numbers(L, 3, "focal", &view.focal, 1);
    numbers(L, 3, "near", &view.near, 1);
    numbers(L, 3, "far", &view.far, 1);
    numbers(L, 3, "sun", view.sun, 3);
    numbers(L, 3, "ambient", &view.ambient, 1);
    numbers(L, 3, "zenith", view.zenith, 3);
    numbers(L, 3, "horizon", view.horizon, 3);
    numbers(L, 3, "haze", view.haze, 3);
    numbers(L, 3, "haze_start", &view.haze_start, 1);
    numbers(L, 3, "haze_end", &view.haze_end, 1);

    /* Chunks are the renderer's: a terrain drawn in another window, or in
     * this one opened again, starts its chunks again. */
    if (g->owner != w || g->serial != w->serial) {
        terrain_free_chunks(g);
        g->owner = w;
        g->serial = w->serial;
        lua_pushvalue(L, 1);
        lua_setiuservalue(L, 2, 1);
    }
    for (int i = 0; i < 3; i++) {
        eye[i] = view.eye[i];
    }
    n = k3d_terrain_choose(&g->place, &g->choice, eye, g->chosen, g->most);
    if (n > w->drawn_room) {
        struct k3d_vk_ground *bigger = realloc(w->drawn, (size_t)n * sizeof(*w->drawn));

        if (bigger == NULL) {
            return luaL_error(L, "out of memory for a frame");
        }
        w->drawn = bigger;
        w->drawn_room = n;
    }
    for (unsigned k = 0; k < n; k++) {
        struct k3d_vk_ground *ground;

        if (chunk_ready(w, g, g->chosen[k], &ground)) {
            w->drawn[drawn++] = *ground;
        } else if (v->why[0]) {
            break;
        }
    }
    if (v->why[0] ||
        (index = kvk_acquire(v, c)) < 0 || !kvk_begin(v) ||
        !k3d_vk_draw(&w->renderer, c->images[index], c->views[index], &view, NULL, 0, w->drawn,
                     drawn)) {
        lua_pushnil(L);
        lua_pushstring(L, v->why[0] ? v->why : "draw");
        v->why[0] = '\0';
        return 2;
    }
    kvk_barrier(v, c->images[index], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    if (!kvk_submit(v) || !kvk_present(v, c, (uint32_t)index)) {
        lua_pushnil(L);
        lua_pushstring(L, v->why);
        v->why[0] = '\0';
        return 2;
    }

    char head[64];
    luaL_buffinit(L, &out);
    snprintf(head, sizeof(head), "drawn=%u of %u ms=%.1f chunks=", drawn, n,
             (seconds() - started) * 1000.0);
    luaL_addstring(&out, head);
    for (unsigned k = 0; k < n; k++) {
        char one[40];

        snprintf(one, sizeof(one), "%s%u/%u/%u", k ? "," : "", g->chosen[k].z, g->chosen[k].x,
                 g->chosen[k].y);
        luaL_addstring(&out, one);
    }
    luaL_pushresult(&out);
    return 1;
}

void kosmos_k3dvk_kit(lua_State *L)
{
    static const luaL_Reg terrain[] = {
        { "wanted", l_terrain_wanted },
        { "elevation", l_terrain_elevation },
        { "imagery", l_terrain_imagery },
        { "__gc", l_terrain_gc },
        { NULL, NULL },
    };
    static const luaL_Reg window[] = {
        { "draw", l_window_draw },
        { "device", l_window_device },
        { "closed", l_window_closed },
        { "close", l_window_close },
        { "__gc", l_window_gc },
        { NULL, NULL },
    };

    luaL_newmetatable(L, TERRAIN);
    luaL_setfuncs(L, terrain, 0);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    luaL_newmetatable(L, WINDOW);
    luaL_setfuncs(L, window, 0);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, l_window);
    lua_setfield(L, -2, "window");
    lua_pushcfunction(L, l_terrain);
    lua_setfield(L, -2, "terrain");
}
