/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The 3D Kit on Vulkan, for Lua** (`docs/flight.md` F4c, F4d): the door
 * an application draws a terrain through - Flight first, and Cafesa3D's
 * Solid view after it. It lives in an image of its own, `k3dvk.elf`, with
 * Mesa linked in, because the system's image does not carry a Vulkan
 * driver; an application says `-- kosmos: image k3dvk.elf` and reaches it
 * as `use("k3dvk.elf")`.
 *
 *     local k = use("k3dvk.elf")
 *     local w = k.window("Flight", 640, 400)
 *     local g = k.terrain{ lon = .., lat = .., root = 12, deepest = 16,
 *                          radius = 2, split = 2, most = 400, elevation = 14,
 *                          keep = 800, uploads = 16 }
 *     for _, t in ipairs(g:wanted(eye)) do ... end   -- { kind, z, x, y }
 *     g:elevation(z, x, y, gfx.png(bytes))          -- a terrarium tile
 *     g:imagery(z, x, y, gfx.jpeg(bytes))           -- the picture over it
 *     local said = w:draw(g, view)                  -- a frame, presented
 *
 * A tile's pixels stay in C: Lua hands over the surface gfx decoded and
 * this reads it where it lies (`kosmos_surface_pixels`).
 *
 * **A chunk is built off the frame's thread** (F4d). Once both its tiles
 * are here - the heights of the elevation tile over it, the picture at its
 * own zoom - a job goes to the terrain's builder, a thread of its own,
 * which makes the mesh; the frame collects what was built and uploads at
 * most `uploads` of them, so a frame's cost does not grow with what the eye
 * has just come upon. A chunk not ready yet is drawn by its nearest ready
 * ancestor, and its ready siblings wait for it, so the ground is never
 * drawn twice in one place and never has a hole where something coarser is
 * ready. And what the eye has left is given back: chunks past `keep`, the
 * least lately drawn first, and heights no chunk is waiting on.
 */

#include <math.h>
#include <pthread.h>
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
#include "k3d_vk.h"
#include "k3d_glb.h"
#include "kits/gfx/picture_decode.h"
#include "storage.h"

#define WINDOW  "k3dvk.window"
#define TERRAIN "k3dvk.terrain"
#define MODEL   "k3dvk.model"
#define KEYCODES 512           /* evdev's, more than a keyboard has */

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

/* An elevation tile's heights. A job holds a copy of `h`, whose samples
 * stay where they are, so `busy` counts the jobs reading them and the
 * heights are not given back while one is. */
struct heights {
    struct k3d_tile at;
    struct map_heights h;
    unsigned busy;
    unsigned used;                  /* the frame that last wanted them */
};

/* A chunk's picture, kept until its chunk is uploaded. */
struct picture {
    struct k3d_tile at;
    uint32_t *pixels;               /* 0xAARRGGBB, packed */
    unsigned width, height;
};

/* A mesh to build, and once built, built: the builder's only business. */
struct job {
    struct job *next;
    struct k3d_place place;
    struct k3d_tile at, from;       /* the chunk, and the elevation tile */
    struct map_heights h;
    struct k3d_terrain_vertex *corners;
    uint32_t *indices;              /* NULL when memory was out */
};

enum { WAITING, BUILDING, MESHED, READY };

struct chunk {
    struct k3d_tile at;
    int state;
    struct job *job;                /* BUILDING and MESHED */
    struct k3d_vk_ground ground;    /* READY */
    unsigned used;                  /* the frame that last drew or chose it */
};

struct window;

struct terrain {
    struct k3d_place place;
    struct k3d_choice choice;
    unsigned most, elevation;       /* chunks at most; the elevation's deepest zoom */
    unsigned keep, uploads;         /* chunks held at most; uploaded in a frame */
    struct heights *heights;
    unsigned nheights, heights_room;
    struct picture *pictures;
    unsigned npictures, pictures_room;
    struct chunk *chunks;
    unsigned nchunks, chunks_room;
    struct k3d_tile *chosen;        /* `most` of them */
    struct k3d_tile *drawn;         /* the frame's, `most` of them */
    unsigned frame, freed;

    /* The builder. `todo` in, `done` out, both under `lock`. */
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_t builder;
    int started, stop;
    struct job *todo, *todo_last, *done;

    /* The window the chunks were uploaded in - held as the terrain's user
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

static struct heights *heights_of(struct terrain *g, struct k3d_tile t)
{
    for (unsigned i = 0; i < g->nheights; i++) {
        if (same(g->heights[i].at, t)) {
            return &g->heights[i];
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

static void job_free(struct job *j)
{
    if (j != NULL) {
        free(j->corners);
        free(j->indices);
        free(j);
    }
}

/* The heights under a place, from the elevation tile over the chunk - a
 * chunk asks only inside its own. */
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

/* The builder: a job at a time, the lock never held while it works. */
static void *builder(void *context)
{
    struct terrain *g = context;

    pthread_mutex_lock(&g->lock);
    for (;;) {
        while (!g->stop && g->todo == NULL) {
            pthread_cond_wait(&g->wake, &g->lock);
        }
        if (g->stop) {
            break;
        }
        struct job *j = g->todo;

        g->todo = j->next;
        if (g->todo == NULL) {
            g->todo_last = NULL;
        }
        pthread_mutex_unlock(&g->lock);

        j->corners = malloc(sizeof(*j->corners) * K3D_CHUNK_VERTICES);
        j->indices = malloc(sizeof(*j->indices) * K3D_CHUNK_INDICES);
        if (j->corners != NULL && j->indices != NULL) {
            struct sampler s = { &j->h, j->from };

            k3d_terrain_chunk(&j->place, j->at, sampled, &s, 20.0f, j->corners, j->indices,
                              NULL, NULL);
        } else {
            free(j->indices);
            j->indices = NULL;
        }

        pthread_mutex_lock(&g->lock);
        j->next = g->done;
        g->done = j;
    }
    pthread_mutex_unlock(&g->lock);
    return NULL;
}

static int send_job(struct terrain *g, struct job *j)
{
    if (!g->started) {
        if (pthread_mutex_init(&g->lock, NULL) != 0 || pthread_cond_init(&g->wake, NULL) != 0 ||
            pthread_create(&g->builder, NULL, builder, g) != 0) {
            return 0;
        }
        g->started = 1;
    }
    j->next = NULL;
    pthread_mutex_lock(&g->lock);
    if (g->todo_last != NULL) {
        g->todo_last->next = j;
    } else {
        g->todo = j;
    }
    g->todo_last = j;
    pthread_cond_signal(&g->wake);
    pthread_mutex_unlock(&g->lock);
    return 1;
}

/* What the builder has finished, made MESHED. */
static void collect(struct terrain *g)
{
    struct job *done;

    if (!g->started) {
        return;
    }
    pthread_mutex_lock(&g->lock);
    done = g->done;
    g->done = NULL;
    pthread_mutex_unlock(&g->lock);

    while (done != NULL) {
        struct job *j = done;
        struct chunk *c = chunk_of(g, j->at);
        struct heights *h = heights_of(g, j->from);

        done = j->next;
        if (h != NULL && h->busy > 0) {
            h->busy--;
        }
        if (c != NULL && c->state == BUILDING && c->job == j && j->indices != NULL) {
            c->state = MESHED;
        } else {
            if (c != NULL && c->job == j) {
                c->job = NULL;
                c->state = WAITING;     /* memory was out: asked again */
            }
            job_free(j);
        }
    }
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

static double field_or(lua_State *L, int table, const char *name, double otherwise)
{
    double value;

    lua_getfield(L, table, name);
    value = luaL_optnumber(L, -1, otherwise);
    lua_pop(L, 1);
    return value;
}

static void terrain_forget_grounds(struct terrain *g);

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
    g->keep = (unsigned)field_or(L, 1, "keep", 2.0 * g->most);
    g->uploads = (unsigned)field_or(L, 1, "uploads", 16);
    /* Not less than `most` needs to be: a chunk of the frame is never given
     * back, so `keep` is how many beyond the frame's are held, at most. */
    luaL_argcheck(L, g->keep >= 1, 1, "keep from 1");
    luaL_argcheck(L, g->uploads >= 1, 1, "uploads from 1");
    g->chosen = calloc(g->most, sizeof(*g->chosen));
    g->drawn = calloc(g->most, sizeof(*g->drawn));
    if (g->chosen == NULL || g->drawn == NULL) {
        return luaL_error(L, "out of memory for a terrain");
    }
    return 1;
}

static int l_terrain_gc(lua_State *L)
{
    struct terrain *g = terrain_check(L, 1);

    if (g->started) {
        pthread_mutex_lock(&g->lock);
        g->stop = 1;
        pthread_cond_broadcast(&g->wake);
        pthread_mutex_unlock(&g->lock);
        pthread_join(g->builder, NULL);
        for (struct job *j = g->todo, *next; j != NULL; j = next) {
            next = j->next;
            job_free(j);
        }
        for (struct job *j = g->done, *next; j != NULL; j = next) {
            next = j->next;
            job_free(j);
        }
        pthread_cond_destroy(&g->wake);
        pthread_mutex_destroy(&g->lock);
    }
    terrain_forget_grounds(g);
    for (unsigned i = 0; i < g->nchunks; i++) {
        /* A BUILDING chunk's job was freed with the lists above. */
        if (g->chunks[i].state == MESHED) {
            job_free(g->chunks[i].job);
        }
    }
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
    free(g->drawn);
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

static void push_tile(lua_State *L, const char *kind, struct k3d_tile t, unsigned *listed)
{
    lua_createtable(L, 4, 0);
    lua_pushstring(L, kind);
    lua_rawseti(L, -2, 1);
    lua_pushinteger(L, t.z);
    lua_rawseti(L, -2, 2);
    lua_pushinteger(L, t.x);
    lua_rawseti(L, -2, 3);
    lua_pushinteger(L, t.y);
    lua_rawseti(L, -2, 4);
    lua_rawseti(L, -2, (lua_Integer)++*listed);
}

/* g:wanted{x, y, z} -> the tiles the chunks for that eye need and have not
 * been given: { "e", z, x, y } and { "i", z, x, y }, each once. A chunk
 * built, or being built, needs nothing. */
static int l_terrain_wanted(lua_State *L)
{
    struct terrain *g = terrain_check(L, 1);
    double eye[3];
    unsigned n, listed = 0;

    eye_of(L, 2, eye);
    n = k3d_terrain_choose(&g->place, &g->choice, eye, g->chosen, g->most);
    lua_newtable(L);
    for (unsigned k = 0; k < n; k++) {
        struct k3d_tile t = g->chosen[k], e = above(t, g->elevation);
        struct chunk *c = chunk_of(g, t);
        int earlier = 0;

        if (c != NULL && c->state != WAITING) {
            continue;
        }
        /* Several chunks share one elevation tile: listed once. */
        for (unsigned j = 0; j < k && !earlier; j++) {
            struct chunk *o = chunk_of(g, g->chosen[j]);

            earlier = (o == NULL || o->state == WAITING) &&
                      same(above(g->chosen[j], g->elevation), e);
        }
        if (heights_of(g, e) == NULL && !earlier) {
            push_tile(L, "e", e, &listed);
        }
        if (picture_of(g, t) == NULL) {
            push_tile(L, "i", t, &listed);
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
/* g:height(x, z) -> the ground's metres at a place in the frame (x east,
 * z south), from the deepest elevation tile held over it - or nil while
 * none is. What an aircraft stands on, lands on and flies into: the
 * same heights the chunks are built from, so it touches the ground it
 * sees. */
static int l_terrain_height(lua_State *L)
{
    struct terrain *g = terrain_check(L, 1);
    double mx = g->place.ox + luaL_checknumber(L, 2) / g->place.metres;
    double my = g->place.oy + luaL_checknumber(L, 3) / g->place.metres;

    if (mx < 0 || mx >= 1 || my < 0 || my >= 1) {
        return 0;
    }
    for (int z = (int)g->elevation; z >= 0; z--) {
        double n = ldexp(1.0, z);
        struct k3d_tile t = { (uint32_t)(mx * n), (uint32_t)(my * n), (uint8_t)z };
        struct heights *h = heights_of(g, t);

        if (h != NULL) {
            lua_pushnumber(L, map_heights_sample(&h->h, mx * n - t.x, my * n - t.y));
            return 1;
        }
    }
    return 0;
}

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
    e->used = g->frame;
    if (!map_heights_terrarium(&e->h, px, w, h, pitch)) {
        return luaL_error(L, "an elevation tile is at least 2 by 2, and needs memory");
    }
    g->nheights++;
    return 0;
}

/* g:imagery(z, x, y, surface): the picture over a tile, kept until its
 * chunk is uploaded. */
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

/* ------------------------------------------------------------- a window */

struct window {
    unsigned serial;                /* 0 once closed */
    struct kvk v;
    struct kw_window *window;
    struct kvk_swapchain chain;
    struct k3d_vk renderer;
    struct k3d_vk_ground *drawn;    /* the frame's, `drawn_room` of them */
    unsigned drawn_room;
    char said[160];
    /* The keys, by keycode: which are down now, and which went down since
     * `pressed` last asked - a bit each, so reading them allocates
     * nothing a frame. */
    uint8_t held[KEYCODES / 8], went_down[KEYCODES / 8];
    int closing;
};

/* Everything the window manager has said, taken in: the keys' transitions
 * into `held` and `went_down`, a close into `closing`. Waits up to
 * `wait` seconds for the first. */
static void take_events(struct window *w, double wait)
{
    double until = seconds() + wait;
    struct kw_event e;

    do {
        while (w->window != NULL && kw_poll(w->window, &e, 10)) {
            if (e.type == KW_CLOSE) {
                w->closing = 1;
            } else if (e.type == KW_RAWKEY && e.key >= 0 && e.key < KEYCODES) {
                uint8_t bit = (uint8_t)(1u << (e.key & 7));

                if (e.action == KW_PRESS) {
                    w->held[e.key >> 3] |= bit;
                    w->went_down[e.key >> 3] |= bit;
                } else {
                    w->held[e.key >> 3] &= (uint8_t)~bit;
                }
            }
        }
    } while (!w->closing && seconds() < until);
}

/* A window's grounds are freed while it is open; once it is closed its
 * device has gone and taken them. Either way the chunks that had them are
 * forgotten, and built again for the next window. */
static void terrain_forget_grounds(struct terrain *g)
{
    struct window *w = g->owner;
    int open = w != NULL && w->serial != 0 && w->serial == g->serial;

    for (unsigned i = 0; i < g->nchunks;) {
        if (g->chunks[i].state == READY) {
            if (open) {
                k3d_vk_ground_free(&w->renderer, &g->chunks[i].ground);
            }
            g->chunks[i] = g->chunks[--g->nchunks];
        } else {
            i++;
        }
    }
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
    static unsigned opened;

    luaL_argcheck(L, width >= 16 && width <= 8192, 2, "a width from 16");
    luaL_argcheck(L, height >= 16 && height <= 8192, 3, "a height from 16");
    hand_console(L);
    w = lua_newuserdatauv(L, sizeof(*w), 0);
    memset(w, 0, sizeof(*w));
    luaL_setmetatable(L, WINDOW);
    w->serial = ++opened ? opened : ++opened;

    const char *why = NULL;
    if (!kvk_open(&w->v, "k3dvk", 1)) {
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

    take_events(w, luaL_optnumber(L, 2, 0.0));
    if (w->closing) {
        window_close(w);
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    return 1;
}

static int keycode(lua_State *L, int at)
{
    lua_Integer code = luaL_checkinteger(L, at);

    luaL_argcheck(L, code >= 0 && code < KEYCODES, at, "a keycode");
    return (int)code;
}

/* w:held(code) -> whether that key is down now, as of the last
 * `closed`: evdev's numbering, as `rawkey` events carry it (`keys.lua`). */
static int l_window_held(lua_State *L)
{
    struct window *w = window_check(L, 1);
    int code = keycode(L, 2);

    lua_pushboolean(L, (w->held[code >> 3] >> (code & 7)) & 1);
    return 1;
}

/* w:pressed(code) -> whether that key went down since this last asked
 * about it: a toggle's question - the view, the flaps, pause - which a
 * key held past one frame must answer once. */
static int l_window_pressed(lua_State *L)
{
    struct window *w = window_check(L, 1);
    int code = keycode(L, 2);
    uint8_t bit = (uint8_t)(1u << (code & 7));
    int was = (w->went_down[code >> 3] & bit) != 0;

    w->went_down[code >> 3] &= (uint8_t)~bit;
    lua_pushboolean(L, was);
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

static void drop_picture(struct terrain *g, struct k3d_tile t)
{
    for (unsigned i = 0; i < g->npictures; i++) {
        if (same(g->pictures[i].at, t)) {
            free(g->pictures[i].pixels);
            g->pictures[i] = g->pictures[--g->npictures];
            return;
        }
    }
}

/* The chosen chunks moved on: a waiting one sent to the builder once its
 * tiles are both here, a built one uploaded while the frame has uploads to
 * spend. 0 with `why` when Vulkan refused an upload. */
static int advance(struct window *w, struct terrain *g, unsigned n, unsigned *uploaded)
{
    /* Every chosen chunk has an entry first, so none moves under a pointer
     * held below. */
    for (unsigned k = 0; k < n; k++) {
        struct chunk *c = chunk_of(g, g->chosen[k]);

        if (c == NULL) {
            if (!room((void **)&g->chunks, &g->chunks_room, g->nchunks, sizeof(*g->chunks))) {
                return 1;
            }
            c = &g->chunks[g->nchunks++];
            memset(c, 0, sizeof(*c));
            c->at = g->chosen[k];
            c->state = WAITING;
        }
        c->used = g->frame;
    }
    /* Coarse before fine: a root's job before its children's, so there is
     * an ancestor to draw while they are built. */
    for (unsigned z = g->choice.root; z <= g->choice.deepest; z++) {
        for (unsigned k = 0; k < n; k++) {
            struct k3d_tile t = g->chosen[k];
            struct chunk *c;

            if (t.z != z || (c = chunk_of(g, t)) == NULL) {
                continue;
            }
            if (c->state == WAITING) {
                struct heights *h = heights_of(g, above(t, g->elevation));
                struct job *j;

                if (h != NULL) {
                    h->used = g->frame;
                }
                if (h == NULL || picture_of(g, t) == NULL ||
                    (j = calloc(1, sizeof(*j))) == NULL) {
                    continue;
                }
                j->place = g->place;
                j->at = t;
                j->from = h->at;
                j->h = h->h;
                if (!send_job(g, j)) {
                    free(j);
                    continue;
                }
                h->busy++;
                c->job = j;
                c->state = BUILDING;
            } else if (c->state == MESHED && *uploaded < g->uploads) {
                struct picture *p = picture_of(g, t);
                struct job *j = c->job;

                if (p == NULL) {
                    continue;           /* given back meanwhile: asked again */
                }
                if (!k3d_vk_ground(&w->renderer, &c->ground, j->corners, K3D_CHUNK_VERTICES,
                                   j->indices, K3D_CHUNK_INDICES, p->pixels, p->width,
                                   p->height, VK_FORMAT_B8G8R8A8_UNORM)) {
                    return 0;
                }
                job_free(j);
                c->job = NULL;
                c->state = READY;
                drop_picture(g, t);
                ++*uploaded;
            }
        }
    }
    return 1;
}

/* The frame's ground: each chosen chunk if it is ready, or else its
 * nearest ready ancestor - and nothing under a tile already drawn. */
static unsigned ground_of(struct terrain *g, unsigned n, unsigned *fallback)
{
    unsigned count = 0;

    *fallback = 0;
    for (unsigned k = 0; k < n; k++) {
        struct k3d_tile t = g->chosen[k];
        int found = 0;

        for (int z = t.z; z >= (int)g->choice.root && !found; z--) {
            struct k3d_tile a = above(t, (unsigned)z);
            struct chunk *c = chunk_of(g, a);

            if (c == NULL || c->state != READY) {
                continue;
            }
            found = 1;
            c->used = g->frame;
            int listed = 0;
            for (unsigned i = 0; i < count && !listed; i++) {
                listed = same(g->drawn[i], a);
            }
            if (!listed && count < g->most) {
                g->drawn[count++] = a;
            }
        }
    }
    /* A tile with an ancestor drawn is covered by it. */
    unsigned kept = 0;
    for (unsigned i = 0; i < count; i++) {
        struct k3d_tile t = g->drawn[i];
        int covered = 0;

        for (unsigned j = 0; j < count && !covered; j++) {
            covered = g->drawn[j].z < t.z && same(above(t, g->drawn[j].z), g->drawn[j]);
        }
        if (!covered) {
            int chosen = 0;

            for (unsigned k = 0; k < n && !chosen; k++) {
                chosen = same(g->chosen[k], t);
            }
            *fallback += !chosen;
            g->drawn[kept++] = t;
        }
    }
    return kept;
}

/* What the eye has left, given back: chunks past `keep`, the least lately
 * used first - never one being built - and heights past `keep` that no
 * job is reading and no chunk of this frame wants. */
static void give_back(struct window *w, struct terrain *g)
{
    while (g->nchunks > g->keep) {
        unsigned oldest = g->nchunks;

        for (unsigned i = 0; i < g->nchunks; i++) {
            struct chunk *c = &g->chunks[i];

            if (c->used != g->frame && c->state != BUILDING &&
                (oldest == g->nchunks || c->used < g->chunks[oldest].used)) {
                oldest = i;
            }
        }
        if (oldest == g->nchunks) {
            break;
        }
        struct chunk *c = &g->chunks[oldest];

        if (c->state == READY) {
            k3d_vk_ground_free(&w->renderer, &c->ground);
        } else if (c->state == MESHED) {
            job_free(c->job);
        }
        drop_picture(g, c->at);
        g->chunks[oldest] = g->chunks[--g->nchunks];
        g->freed++;
    }
    while (g->nheights > g->keep) {
        unsigned oldest = g->nheights;

        for (unsigned i = 0; i < g->nheights; i++) {
            struct heights *h = &g->heights[i];

            if (h->busy == 0 && h->used != g->frame &&
                (oldest == g->nheights || h->used < g->heights[oldest].used)) {
                oldest = i;
            }
        }
        if (oldest == g->nheights) {
            break;
        }
        map_heights_free(&g->heights[oldest].h);
        g->heights[oldest] = g->heights[--g->nheights];
    }
}

/* w:draw(terrain, view) -> "drawn=D of N ready=R building=B fallback=F
 * held=H freed=X uploads=U ms=T chunks=z/x/y,...", a frame drawn and
 * presented, `chunks` the ground it drew; nil and why when it could not be. */
struct model;
static struct model *model_check(lua_State *L, int index);
static const struct k3d_vk_model_draw *model_draws(struct model *m, uint32_t *n);

static int l_window_draw(lua_State *L)
{
    struct window *w = window_check(L, 1);
    struct terrain *g = terrain_check(L, 2);
    struct kvk *v = &w->v;
    struct kvk_swapchain *c = &w->chain;
    struct k3d_vk_view view;
    double eye[3], started = seconds();
    unsigned n, count, fallback, ready = 0, building = 0, uploaded = 0;
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
    /* The camera's up, when it rolls - the cockpit's; the world's without. */
    lua_getfield(L, 3, "up");
    if (!lua_isnil(L, -1)) {
        lua_pop(L, 1);
        numbers(L, 3, "up", view.up, 3);
    } else {
        lua_pop(L, 1);
    }

    /* Grounds are the renderer's: a terrain drawn in another window, or in
     * this one opened again, uploads its chunks again. */
    if (g->owner != w || g->serial != w->serial) {
        terrain_forget_grounds(g);
        g->owner = w;
        g->serial = w->serial;
        lua_pushvalue(L, 1);
        lua_setiuservalue(L, 2, 1);
    }
    g->frame++;
    for (int i = 0; i < 3; i++) {
        eye[i] = view.eye[i];
    }
    collect(g);
    n = k3d_terrain_choose(&g->place, &g->choice, eye, g->chosen, g->most);
    if (!advance(w, g, n, &uploaded)) {
        lua_pushnil(L);
        lua_pushstring(L, v->why);
        v->why[0] = '\0';
        return 2;
    }
    count = ground_of(g, n, &fallback);
    for (unsigned k = 0; k < n; k++) {
        struct chunk *ch = chunk_of(g, g->chosen[k]);

        ready += ch != NULL && ch->state == READY;
        building += ch != NULL && (ch->state == BUILDING || ch->state == MESHED);
    }
    if (count > w->drawn_room) {
        struct k3d_vk_ground *bigger = realloc(w->drawn, (size_t)count * sizeof(*w->drawn));

        if (bigger == NULL) {
            return luaL_error(L, "out of memory for a frame");
        }
        w->drawn = bigger;
        w->drawn_room = count;
    }
    for (unsigned k = 0; k < count; k++) {
        w->drawn[k] = chunk_of(g, g->drawn[k])->ground;
    }
    /* The aircraft, or whatever model the frame is given. */
    const struct k3d_vk_model_draw *plane = NULL;
    uint32_t nplane = 0;

    if (!lua_isnoneornil(L, 4)) {
        plane = model_draws(model_check(L, 4), &nplane);
    }
    if ((index = kvk_acquire(v, c)) < 0 || !kvk_begin(v) ||
        !k3d_vk_draw_models(&w->renderer, c->images[index], c->views[index], &view, NULL, 0,
                            w->drawn, count, plane, nplane)) {
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
    give_back(w, g);

    char head[160];
    luaL_buffinit(L, &out);
    snprintf(head, sizeof(head),
             "drawn=%u of %u ready=%u building=%u fallback=%u held=%u freed=%u uploads=%u "
             "ms=%.1f chunks=", count, n, ready, building, fallback, g->nchunks, g->freed,
             uploaded, (seconds() - started) * 1000.0);
    luaL_addstring(&out, head);
    for (unsigned k = 0; k < count; k++) {
        char one[40];

        snprintf(one, sizeof(one), "%s%u/%u/%u", k ? "," : "", g->drawn[k].z, g->drawn[k].x,
                 g->drawn[k].y);
        luaL_addstring(&out, one);
    }
    luaL_pushresult(&out);
    return 1;
}

/*
 * **A model**: a glTF binary read by the 3D Kit (`k3d_glb.c`) and put on
 * the window's device - a renderer model a part and a texture a picture -
 * drawn each frame where `place` says, its movable parts each turned about
 * their hinge by `turn`. The file is read here, in C, through the Storage
 * Kit: the Cessna is seventeen megabytes and a process's Lua heap two, so
 * none of it is a Lua string; its pictures are decoded by gfx's cores
 * (`picture_decode.c`) and given to the GPU, and only what is drawn stays.
 */
struct model {
    unsigned serial;                /* the window's; 0 once freed */
    struct window *owner;
    struct k3d_glb glb;
    struct k3d_vk_model *parts;     /* glb.nparts */
    struct k3d_vk_texture *textures;/* glb.ntextures; set == 0: none */
    float place[16];                /* the world from the model */
    float angle[K3D_GLB_MOVABLES];  /* radians about each movable's hinge */
    bool spins[K3D_GLB_MOVABLES];   /* about its own Z rather than its hinge */
    struct k3d_vk_model_draw *draws;/* the frame's, glb.nparts */
};

_Static_assert(sizeof(struct k3d_glb_vertex) == sizeof(struct k3d_vk_model_vertex),
               "the GLB reader's vertex is the renderer's");

static void model_free(struct model *m)
{
    struct window *w = m->owner;
    int open = w != NULL && w->serial != 0 && w->serial == m->serial;

    if (open) {
        w->v.vkDeviceWaitIdle(w->v.device);
        for (uint32_t i = 0; m->parts != NULL && i < m->glb.nparts; i++) {
            k3d_vk_model_free(&w->renderer, &m->parts[i]);
        }
        for (uint32_t i = 0; m->textures != NULL && i < m->glb.ntextures; i++) {
            if (m->textures[i].set != VK_NULL_HANDLE) {
                k3d_vk_texture_free(&w->renderer, &m->textures[i]);
            }
        }
    }
    free(m->parts);
    free(m->textures);
    free(m->draws);
    k3d_glb_free(&m->glb);
    memset(m, 0, sizeof(*m));
}

static int l_model_gc(lua_State *L)
{
    model_free(luaL_checkudata(L, 1, MODEL));
    return 0;
}

static struct model *model_check(lua_State *L, int index)
{
    struct model *m = luaL_checkudata(L, index, MODEL);

    if (m->serial == 0 || m->owner == NULL || m->owner->serial != m->serial) {
        luaL_error(L, "the model's window is closed");
    }
    return m;
}

/* The whole file, read through the Storage Kit; NULL and why. */
static uint8_t *read_whole(const char *path, size_t *n, const char **why)
{
    struct storage_stat st;
    uint8_t *bytes;

    if (storage_stat(path, &st) != STORAGE_OK || st.kind != STORAGE_KIND_FILE) {
        *why = "no such file";
        return NULL;
    }
    if ((bytes = malloc(st.size ? st.size : 1)) == NULL) {
        *why = "no memory for the file";
        return NULL;
    }
    for (uint64_t at = 0; at < st.size;) {
        long got = storage_read(path, at, bytes + at, (size_t)(st.size - at));

        if (got <= 0) {
            free(bytes);
            *why = "the file could not be read";
            return NULL;
        }
        at += (uint64_t)got;
    }
    *n = (size_t)st.size;
    return bytes;
}

/* w:model(path, {names...}) -> a model, or nil and why. `names` are the
 * nodes that move - a control surface, a propeller - in the order `turn`
 * numbers them from 1. */
static int l_window_model(lua_State *L)
{
    struct window *w = window_check(L, 1);
    const char *path = luaL_checkstring(L, 2);
    const char *names[K3D_GLB_MOVABLES];
    unsigned count = 0;
    const char *why = NULL;
    size_t n = 0;
    uint8_t *bytes;
    struct model *m;

    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        count = (unsigned)lua_rawlen(L, 3);
        luaL_argcheck(L, count <= K3D_GLB_MOVABLES, 3, "at most 16 names");
        for (unsigned i = 0; i < count; i++) {
            lua_rawgeti(L, 3, (lua_Integer)i + 1);
            names[i] = luaL_checkstring(L, -1);
            lua_pop(L, 1);          /* the table keeps the string alive */
        }
    }

    m = lua_newuserdatauv(L, sizeof(*m), 1);
    memset(m, 0, sizeof(*m));
    luaL_setmetatable(L, MODEL);
    lua_pushvalue(L, 1);
    lua_setiuservalue(L, -2, 1);    /* the window outlives its models */

    if ((bytes = read_whole(path, &n, &why)) == NULL) {
        goto refused;
    }
    if (!k3d_glb_read(bytes, n, names, count, &m->glb, &why)) {
        free(bytes);
        goto refused;
    }
    m->owner = w;
    m->serial = w->serial;
    m->parts = calloc(m->glb.nparts ? m->glb.nparts : 1, sizeof(*m->parts));
    m->draws = calloc(m->glb.nparts ? m->glb.nparts : 1, sizeof(*m->draws));
    m->textures = calloc(m->glb.ntextures ? m->glb.ntextures : 1, sizeof(*m->textures));
    if (m->parts == NULL || m->draws == NULL || m->textures == NULL) {
        free(bytes);
        why = "no memory for the model";
        goto refused;
    }
    /* Only the pictures a part is drawn with - its base colour. A glTF
     * carries normal and roughness maps as well, which this renderer does
     * not read yet: the Cessna's 28 are 10 it uses, and decoding the other
     * 18 was memory spent on nothing. */
    for (uint32_t i = 0; i < m->glb.ntextures; i++) {
        const struct k3d_glb_texture *t = &m->glb.textures[i];
        uint32_t *px = NULL;
        unsigned tw = 0, th = 0;
        const char *said = NULL;
        bool used = false;

        for (uint32_t k = 0; k < m->glb.nparts && !used; k++) {
            used = m->glb.parts[k].texture == (int)i;
        }
        if (t->length == 0 || !used) {
            continue;
        }
        if (gfx_png_decode(bytes + t->offset, t->length, &px, &tw, &th, &said) != GFX_PICTURE_OK &&
            gfx_jpeg_decode(bytes + t->offset, t->length, &px, &tw, &th, &said) != GFX_PICTURE_OK) {
            continue;               /* drawn in its colour alone */
        }
        if (!k3d_vk_texture(&w->renderer, &m->textures[i], px, tw, th, VK_FORMAT_B8G8R8A8_UNORM)) {
            memset(&m->textures[i], 0, sizeof(m->textures[i]));
        }
        gfx_picture_free(px);
    }
    free(bytes);                    /* the textures are on the device now */
    for (uint32_t i = 0; i < m->glb.nparts; i++) {
        const struct k3d_glb_part *p = &m->glb.parts[i];

        if (!k3d_vk_model(&w->renderer, &m->parts[i],
                          (const struct k3d_vk_model_vertex *)p->vertices, p->nvertices,
                          p->indices, p->nindices)) {
            why = w->v.why[0] ? w->v.why : "a part would not go on the device";
            goto refused;
        }
    }
    for (int i = 0; i < 16; i++) {
        m->place[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    return 1;

refused:;
    char copy[160];

    snprintf(copy, sizeof(copy), "%s", why ? why : "refused");
    model_free(m);
    lua_pushnil(L);
    lua_pushstring(L, copy);
    return 2;
}

/* m:place(t): the world from the model, sixteen numbers column-major in a
 * table the caller keeps and refills - no table made a frame. */
static int l_model_place(lua_State *L)
{
    struct model *m = model_check(L, 1);

    luaL_checktype(L, 2, LUA_TTABLE);
    for (int i = 0; i < 16; i++) {
        lua_rawgeti(L, 2, i + 1);
        m->place[i] = (float)luaL_checknumber(L, -1);
        lua_pop(L, 1);
    }
    return 0;
}

/* m:turn(i, radians): the i-th named part turned about its hinge. */
static int turn(lua_State *L, bool spin)
{
    struct model *m = model_check(L, 1);
    lua_Integer i = luaL_checkinteger(L, 2);

    luaL_argcheck(L, i >= 1 && i <= (lua_Integer)m->glb.nmovables, 2, "a named part");
    m->angle[i - 1] = (float)luaL_checknumber(L, 3);
    m->spins[i - 1] = spin;
    return 0;
}

static int l_model_turn(lua_State *L) { return turn(L, false); }

/* m:spin(i, radians): the same about the part's own Z - a propeller's. */
static int l_model_spin(lua_State *L) { return turn(L, true); }

/* m:info() -> "parts=16 triangles=77317 textures=28 size=8.15x2.70x10.92" */
static int l_model_info(lua_State *L)
{
    struct model *m = model_check(L, 1);
    unsigned textured = 0, used = 0;

    for (uint32_t i = 0; i < m->glb.ntextures; i++) {
        bool wanted = false;

        for (uint32_t k = 0; k < m->glb.nparts && !wanted; k++) {
            wanted = m->glb.parts[k].texture == (int)i;
        }
        used += wanted;
        textured += m->textures[i].set != VK_NULL_HANDLE;
    }
    char said[160];

    snprintf(said, sizeof(said), "parts=%u triangles=%u textures=%u of %u used, %u in the file "
             "size=%.2fx%.2fx%.2f", (unsigned)m->glb.nparts, (unsigned)m->glb.triangles, textured,
             used, (unsigned)m->glb.ntextures, m->glb.hi[0] - m->glb.lo[0],
             m->glb.hi[1] - m->glb.lo[1], m->glb.hi[2] - m->glb.lo[2]);
    lua_pushstring(L, said);
    return 1;
}

static void multiply(const float a[16], const float b[16], float out[16])
{
    float r[16];

    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0;

            for (int k = 0; k < 4; k++) {
                sum += a[k * 4 + row] * b[c * 4 + k];
            }
            r[c * 4 + row] = sum;
        }
    }
    memcpy(out, r, sizeof(r));
}

/* A turn of `angle` about the line through `pivot` along unit `axis`. */
static void hinge_turn(const float pivot[3], const float axis[3], float angle, float out[16])
{
    float c = cosf(angle), s = sinf(angle), t = 1 - c;
    float x = axis[0], y = axis[1], z = axis[2];
    float r[9] = { t * x * x + c,     t * x * y + s * z, t * x * z - s * y,
                   t * x * y - s * z, t * y * y + c,     t * y * z + s * x,
                   t * x * z + s * y, t * y * z - s * x, t * z * z + c };

    for (int col = 0; col < 3; col++) {
        for (int row = 0; row < 3; row++) {
            out[col * 4 + row] = r[col * 3 + row];
        }
        out[col * 4 + 3] = 0;
    }
    /* p - R p: the pivot stays where it is. */
    for (int row = 0; row < 3; row++) {
        out[12 + row] = pivot[row] - (r[row] * pivot[0] + r[3 + row] * pivot[1] +
                                      r[6 + row] * pivot[2]);
    }
    out[15] = 1;
}

/* The frame's draws for a model: each part where the model is, its own
 * matrix after, and a movable part turned about its hinge between. */
static const struct k3d_vk_model_draw *model_draws(struct model *m, uint32_t *n)
{
    float turned[K3D_GLB_MOVABLES][16];

    for (unsigned i = 0; i < m->glb.nmovables; i++) {
        const struct k3d_glb_movable *mv = &m->glb.movables[i];
        /* A propeller spins about its node's own Z, the line of flight
         * (`k3d_glb.h`): its shape cannot say so. The rest about the
         * hinge they are longest along. */
        const float *axis = m->spins[i] ? mv->axes[2] : mv->hinge;

        hinge_turn(mv->pivot, axis, m->angle[i], turned[i]);
    }
    for (uint32_t i = 0; i < m->glb.nparts; i++) {
        const struct k3d_glb_part *p = &m->glb.parts[i];
        struct k3d_vk_model_draw *d = &m->draws[i];
        float at[16];

        d->model = &m->parts[i];
        d->texture = (p->texture >= 0 && (uint32_t)p->texture < m->glb.ntextures &&
                      m->textures[p->texture].set != VK_NULL_HANDLE)
                         ? &m->textures[p->texture] : NULL;
        memcpy(d->tint, p->colour, sizeof(d->tint));
        if (p->movable >= 0 && (unsigned)p->movable < m->glb.nmovables) {
            multiply(turned[p->movable], p->matrix, at);
        } else {
            memcpy(at, p->matrix, sizeof(at));
        }
        multiply(m->place, at, d->matrix);
    }
    *n = m->glb.nparts;
    return m->draws;
}

void kosmos_k3dvk_kit(lua_State *L)
{
    static const luaL_Reg terrain[] = {
        { "wanted", l_terrain_wanted },
        { "elevation", l_terrain_elevation },
        { "imagery", l_terrain_imagery },
        { "height", l_terrain_height },
        { "__gc", l_terrain_gc },
        { NULL, NULL },
    };
    static const luaL_Reg window[] = {
        { "draw", l_window_draw },
        { "device", l_window_device },
        { "closed", l_window_closed },
        { "held", l_window_held },
        { "pressed", l_window_pressed },
        { "model", l_window_model },
        { "close", l_window_close },
        { "__gc", l_window_gc },
        { NULL, NULL },
    };

    luaL_newmetatable(L, TERRAIN);
    luaL_setfuncs(L, terrain, 0);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    static const luaL_Reg model[] = {
        { "place", l_model_place },
        { "turn", l_model_turn },
        { "spin", l_model_spin },
        { "info", l_model_info },
        { "__gc", l_model_gc },
        { NULL, NULL },
    };

    luaL_newmetatable(L, MODEL);
    luaL_setfuncs(L, model, 0);
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
