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
 *     g:invent("e" or "i", z, x, y)                 -- an invented island's, instead
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
#include "k3d_island.h"
#include "k3d_extrude.h"
#include "kits/map/mvt.h"
#include "kits/compress/gzip.h"
#include "kits/gfx/picture_decode.h"
#include "storage.h"

#define WINDOW  "k3dvk.window"
#define TERRAIN "k3dvk.terrain"
#define MODEL   "k3dvk.model"
#define KEYCODES 512           /* evdev's, more than a keyboard has */
#define OVERLAYS 6             /* a window's overlay places: a strip, a panel, a sheet */
#define TYPED 64               /* characters held for `w:typed`, as UTF-8 bytes */

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
    const struct k3d_flat *flats;   /* the terrain's, never changed after it is made */
    unsigned nflats;
    struct k3d_terrain_vertex *corners;
    uint32_t *indices;              /* NULL when memory was out */
    /* A tile's buildings instead (B3): its vector tile's bytes, the
     * terrain's, read and not freed by the builder; what they made. */
    int buildings;
    const uint8_t *bytes;
    size_t nbytes;
    struct k3d_extruded solid;
    struct box *boxes;              /* each building's extent and top */
    size_t nboxes, boxes_room;
    unsigned made, refused;
    int failed;                     /* not a vector tile, or memory was out */
};

enum { WAITING, BUILDING, MESHED, READY };

struct chunk {
    struct k3d_tile at;
    int state;
    struct job *job;                /* BUILDING and MESHED */
    struct k3d_vk_ground ground;    /* READY */
    unsigned used;                  /* the frame that last drew or chose it */
};

/* A building's extent on the ground and its roof's height, in the frame's
 * metres: what `g:top` answers from. */
struct box {
    float x0, z0, x1, z1, top;
};

/*
 * **A tile's buildings** (`docs/flight.md` F6, B3): the map's vector tile
 * at zoom `buildings`, its footprints stood up by the builder from the
 * lowest ground under each, and put on the device as one model - so a
 * city's thousands of buildings are one draw. Its bytes are held while the
 * tile is, so a window opened again builds it again without asking.
 */
struct btile {
    struct k3d_tile at;
    int state;                      /* WAITING with or without bytes, BUILDING,
                                     * MESHED, READY */
    uint8_t *bytes;                 /* NULL until they come */
    size_t nbytes;
    struct job *job;                /* BUILDING and MESHED */
    struct k3d_vk_model model;      /* READY, when `triangles`: the walls */
    struct k3d_vk_model roofs;      /* READY, when `roof_triangles` */
    struct box *boxes;              /* from MESHED on, kept with the bytes */
    size_t nboxes;
    unsigned made, refused, triangles, roof_triangles;
    /* The photograph over it (B4): `pieces` imagery tiles at the roofs'
     * zoom, each ROOF_PIECE square, laid into one picture as they come;
     * on the device once all have. */
    uint32_t *roof_pixels;
    uint32_t pieces;                /* a bit for each piece held */
    uint32_t photos;                /* and for each that is a photograph,
                                     * not the concrete a missing one is */
    struct k3d_vk_texture roof_texture;
    int textured;
    unsigned used;
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
    struct k3d_flat *flats;         /* the runways' ground, given when it is made */
    unsigned nflats;
    /* The buildings: their zoom (0, none), how many tiles out from the
     * eye's, how many held at most; and the builder's inflater. */
    unsigned bzoom, bradius, bkeep;
    unsigned rzoom;                 /* the roofs' imagery zoom; 0, plain */
    struct btile *btiles;
    unsigned nbtiles, btiles_room;
    struct gunzip_work *work;

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
        k3d_extruded_free(&j->solid);
        free(j->boxes);
        free(j);
    }
}

static struct btile *btile_of(struct terrain *g, struct k3d_tile t)
{
    for (unsigned i = 0; i < g->nbtiles; i++) {
        if (same(g->btiles[i].at, t)) {
            return &g->btiles[i];
        }
    }
    return NULL;
}

/* The heights under a place, from the elevation tile over the chunk - a
 * chunk asks only inside its own. */
struct sampler {
    const struct map_heights *h;
    struct k3d_tile at;
    const struct k3d_flat *flats;
    unsigned nflats;
};

static float sampled(void *context, double mx, double my)
{
    const struct sampler *s = context;
    double n = ldexp(1.0, s->at.z);

    return k3d_flatten(s->flats, s->nflats, mx, my,
                       map_heights_sample(s->h, mx * n - s->at.x, my * n - s->at.y));
}

/*
 * **A tile's buildings, stood up** (B3), on the builder's thread. The
 * vector tile is inflated if it came gzipped, decoded by the Map Kit's
 * reader, and each polygon of its `building` layer - an outer ring and the
 * holes after it, the outer's winding telling which is which - is made
 * solid by the 3D Kit's extruder in the frame's metres. A building stands
 * on the lowest ground under its outer ring, the same heights the chunks
 * are built from, so none floats on a slope; one whose middle is past the
 * tile's edge is its neighbour's, which has it too, so none is drawn twice.
 */
struct inflated {
    uint8_t *bytes;
    size_t n, room;
    int short_of_memory;
};

static int inflated_put(void *user, const uint8_t *bytes, size_t n)
{
    struct inflated *o = user;

    if (o->n + n > o->room) {
        size_t room = o->room ? o->room : 256 * 1024;
        uint8_t *bigger;

        while (room < o->n + n) {
            room *= 2;
        }
        if (room > 64u << 20 || (bigger = realloc(o->bytes, room)) == NULL) {
            o->short_of_memory = 1;
            return 0;
        }
        o->bytes = bigger;
        o->room = room;
    }
    memcpy(o->bytes + o->n, bytes, n);
    o->n += n;
    return 1;
}

/* Twice a ring's signed area, in the tile's units. */
static double ring_area(const struct mvt_tile *t, const struct mvt_part *r)
{
    const float *q = &t->xy[2 * r->first];
    double a = 0;

    for (uint32_t i = 0; i < r->count; i++) {
        uint32_t k = (i + 1) % r->count;

        a += (double)q[2 * i] * q[2 * k + 1] - (double)q[2 * k] * q[2 * i + 1];
    }
    return a;
}

#define BUILDING_HEIGHT 6.0f
#define ROOF_PIECE 256              /* texels on a side of one roof piece */        /* metres, for a footprint that says none */

static void build_buildings(struct job *j, struct gunzip_work **work)
{
    const uint8_t *bytes = j->bytes;
    size_t n = j->nbytes, got;
    struct inflated in = { 0 };
    struct mvt_tile t;
    struct sampler sm = { &j->h, j->from, j->flats, j->nflats };
    double *xz = NULL;
    struct k3d_ring *rings = NULL;
    size_t xz_room = 0, rings_room = 0;

    memset(&t, 0, sizeof(t));
    if (n >= 2 && bytes[0] == 0x1f && bytes[1] == 0x8b) {
        if (*work == NULL && (*work = malloc(sizeof(**work))) == NULL) {
            j->failed = 1;
            return;
        }
        if (kosmos_gunzip(bytes, n, *work, inflated_put, &in, &got) != GUNZIP_WHOLE) {
            free(in.bytes);
            j->failed = 1;
            return;
        }
        bytes = in.bytes, n = in.n;
    }
    if (mvt_decode(&t, bytes, n) != MVT_OK) {
        mvt_free(&t);
        free(in.bytes);
        j->failed = 1;
        return;
    }
    free(in.bytes);

    double side = (double)t.extent * ldexp(1.0, j->at.z);

    for (size_t fi = 0; fi < t.nfeatures; fi++) {
        const struct mvt_feature *f = &t.features[fi];
        uint32_t p = 0;
        double outer_sign;

        if (f->layer != L_BUILDING || f->type != MVT_POLYGON || f->nparts == 0) {
            continue;
        }
        outer_sign = ring_area(&t, &t.parts[f->part0]);

        while (p < f->nparts) {
            /* One polygon: the ring at `p` and the holes after it, which
             * wind the other way. */
            uint32_t first = p, points = t.parts[f->part0 + p].count;

            for (p++; p < f->nparts &&
                      (ring_area(&t, &t.parts[f->part0 + p]) > 0) != (outer_sign > 0); p++) {
                points += t.parts[f->part0 + p].count;
            }

            unsigned nrings = p - first;
            const struct mvt_part *outer = &t.parts[f->part0 + first];
            double cx = 0, cy = 0;

            if (outer->count < 3) {
                continue;
            }
            for (uint32_t i = 0; i < outer->count; i++) {
                cx += t.xy[2 * (outer->first + i)];
                cy += t.xy[2 * (outer->first + i) + 1];
            }
            cx /= outer->count, cy /= outer->count;
            if (cx < 0 || cy < 0 || cx >= t.extent || cy >= t.extent) {
                continue;                   /* the neighbour's */
            }
            if (points * 2 > xz_room) {
                double *bigger = realloc(xz, sizeof(*xz) * points * 2);

                if (bigger == NULL) {
                    j->failed = 1;
                    break;
                }
                xz = bigger, xz_room = points * 2;
            }
            if (nrings > rings_room) {
                struct k3d_ring *bigger = realloc(rings, sizeof(*rings) * nrings);

                if (bigger == NULL) {
                    j->failed = 1;
                    break;
                }
                rings = bigger, rings_room = nrings;
            }

            float ground = INFINITY;
            size_t at = 0;

            for (unsigned r = 0; r < nrings; r++) {
                const struct mvt_part *pt = &t.parts[f->part0 + first + r];

                rings[r].xz = &xz[at];
                rings[r].n = pt->count;
                for (uint32_t i = 0; i < pt->count; i++) {
                    double mx = (j->at.x * (double)t.extent + t.xy[2 * (pt->first + i)]) / side;
                    double my = (j->at.y * (double)t.extent + t.xy[2 * (pt->first + i) + 1]) / side;

                    k3d_place_world(&j->place, mx, my, &xz[at], &xz[at + 1]);
                    at += 2;
                    if (r == 0) {
                        float h = sampled(&sm, mx, my);

                        if (h < ground) {
                            ground = h;
                        }
                    }
                }
            }
            float tall = f->height > 0 ? f->height : BUILDING_HEIGHT;
            float from = f->min_height > 0 && f->min_height < tall ? f->min_height : 0;

            if (isfinite(ground) &&
                k3d_extrude(&j->solid, rings, nrings, 0, 0, ground + from, ground + tall)) {
                j->made++;
                if (j->nboxes == j->boxes_room) {
                    size_t more = j->boxes_room ? j->boxes_room * 2 : 256;
                    struct box *bigger = realloc(j->boxes, more * sizeof(*bigger));

                    if (bigger != NULL) {
                        j->boxes = bigger, j->boxes_room = more;
                    }
                }
                if (j->nboxes < j->boxes_room) {
                    struct box *bx = &j->boxes[j->nboxes++];

                    bx->x0 = bx->z0 = INFINITY, bx->x1 = bx->z1 = -INFINITY;
                    bx->top = ground + tall;
                    for (unsigned i = 0; i < rings[0].n; i++) {
                        float x = (float)rings[0].xz[2 * i], z = (float)rings[0].xz[2 * i + 1];

                        bx->x0 = fminf(bx->x0, x), bx->x1 = fmaxf(bx->x1, x);
                        bx->z0 = fminf(bx->z0, z), bx->z1 = fmaxf(bx->z1, z);
                    }
                }
            } else {
                j->refused++;
            }
        }
        if (j->failed) {
            break;
        }
    }
    /* A roof's (u, v) across the tile, 0 to 1 - where on the tile's
     * photograph it stands (B4) - from the frame's metres it was given. */
    for (size_t i = 0; i < j->solid.nroof; i++) {
        struct k3d_extrude_vertex *v = &j->solid.roof[i];
        double n = ldexp(1.0, j->at.z);
        double u = (v->position[0] / j->place.metres + j->place.ox) * n - j->at.x;
        double w = (v->position[2] / j->place.metres + j->place.oy) * n - j->at.y;

        v->uv[0] = (float)fmin(1.0, fmax(0.0, u));
        v->uv[1] = (float)fmin(1.0, fmax(0.0, w));
    }
    free(xz);
    free(rings);
    mvt_free(&t);
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

        if (j->buildings) {
            build_buildings(j, &g->work);
        } else if ((j->corners = malloc(sizeof(*j->corners) * K3D_CHUNK_VERTICES)) != NULL &&
                   (j->indices = malloc(sizeof(*j->indices) * K3D_CHUNK_INDICES)) != NULL) {
            struct sampler s = { &j->h, j->from, j->flats, j->nflats };

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
        if (j->buildings) {
            struct btile *b = btile_of(g, j->at);

            if (b != NULL && b->state == BUILDING && b->job == j) {
                b->made = j->made;
                b->refused = j->refused;
                free(b->boxes);
                b->boxes = j->boxes, b->nboxes = j->nboxes;
                j->boxes = NULL, j->nboxes = 0;
                /* A tile that would not decode is kept as one with none,
                 * and not asked for again. */
                b->state = MESHED;
            } else {
                job_free(j);
            }
            continue;
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
    /* The buildings (B3): `buildings = 14` the vector tiles' zoom, at or
     * deeper than the elevation's so one elevation tile is under each;
     * `buildings_radius` tiles out from the eye's each way. */
    g->bzoom = (unsigned)field_or(L, 1, "buildings", 0);
    g->bradius = (unsigned)field_or(L, 1, "buildings_radius", 1);
    luaL_argcheck(L, g->bzoom == 0 || (g->bzoom >= g->elevation && g->bzoom <= 16), 1,
                  "buildings at a zoom from the elevation's to 16");
    luaL_argcheck(L, g->bradius <= 4, 1, "buildings_radius at most 4");
    g->bkeep = (2 * g->bradius + 1) * (2 * g->bradius + 1) * 2;
    /* `roofs = 16`: the roofs wear the imagery at that zoom, up to two
     * deeper than the buildings' - 16 pieces, a picture of 1024. */
    g->rzoom = (unsigned)field_or(L, 1, "roofs", 0);
    luaL_argcheck(L, g->rzoom == 0 || (g->bzoom > 0 && g->rzoom >= g->bzoom &&
                                       g->rzoom <= g->bzoom + 2), 1,
                  "roofs at the buildings' zoom or up to two deeper");
    g->chosen = calloc(g->most, sizeof(*g->chosen));
    g->drawn = calloc(g->most, sizeof(*g->drawn));
    if (g->chosen == NULL || g->drawn == NULL) {
        return luaL_error(L, "out of memory for a terrain");
    }
    /*
     * **The runways' ground** (F8): `flats = { { x1, z1, x2, z2, width,
     * h1, h2 }, ... }`, each a runway's ends in the frame's metres, its
     * width and the heights at its ends. Given here and never after, so the
     * builder's thread reads them without a lock: a new place is a new
     * terrain. The ground is the runway's for half its width and 30 m
     * beside it, and blends back to the tiles' over 150 m more - three
     * samples of the finest chunk at least, so a runway is level whatever
     * the samples' spacing.
     */
    lua_getfield(L, 1, "flats");
    if (lua_type(L, -1) == LUA_TTABLE) {
        lua_Integer count = luaL_len(L, -1);

        luaL_argcheck(L, count >= 0 && count <= 4096, 1, "at most 4096 flats");
        if (count > 0) {
            g->flats = calloc((size_t)count, sizeof(*g->flats));
            if (g->flats == NULL) {
                return luaL_error(L, "out of memory for a terrain's flats");
            }
        }
        for (lua_Integer i = 1; i <= count; i++) {
            double v[7];

            lua_geti(L, -1, i);
            luaL_argcheck(L, lua_type(L, -1) == LUA_TTABLE, 1, "a flat is a table of 7 numbers");
            for (int k = 0; k < 7; k++) {
                lua_geti(L, -1, k + 1);
                luaL_argcheck(L, lua_type(L, -1) == LUA_TNUMBER, 1,
                              "a flat is x1, z1, x2, z2, width, h1, h2");
                v[k] = lua_tonumber(L, -1);
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
            luaL_argcheck(L, v[4] > 0 && v[4] < 1000, 1, "a flat's width from 0 to 1000 m");
            struct k3d_flat *f = &g->flats[g->nflats++];

            f->ax = g->place.ox + v[0] / g->place.metres;
            f->ay = g->place.oy + v[1] / g->place.metres;
            f->bx = g->place.ox + v[2] / g->place.metres;
            f->by = g->place.oy + v[3] / g->place.metres;
            f->half = (v[4] / 2 + 30) / g->place.metres;
            f->margin = 150 / g->place.metres;
            f->ha = (float)v[5];
            f->hb = (float)v[6];
        }
    }
    lua_pop(L, 1);
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
    for (unsigned i = 0; i < g->nbtiles; i++) {
        if (g->btiles[i].state == MESHED) {
            job_free(g->btiles[i].job);
        }
        free(g->btiles[i].bytes);
        free(g->btiles[i].boxes);
        free(g->btiles[i].roof_pixels);
    }
    free(g->btiles);
    free(g->work);
    for (unsigned i = 0; i < g->nheights; i++) {
        map_heights_free(&g->heights[i].h);
    }
    for (unsigned i = 0; i < g->npictures; i++) {
        free(g->pictures[i].pixels);
    }
    free(g->heights);
    free(g->pictures);
    free(g->chunks);
    free(g->flats);
    g->flats = NULL;
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

/* Every piece of a tile's roof picture, as `pieces` holds them. */
static uint32_t roof_all(const struct terrain *g)
{
    unsigned k = 1u << (g->rzoom - g->bzoom);

    return k * k >= 32 ? 0xFFFFFFFFu : (1u << (k * k)) - 1;
}

/* The building tiles within `bradius` of the eye's, nearest first: how
 * many, at most 81. */
static unsigned buildings_about(const struct terrain *g, const double eye[3],
                                struct k3d_tile *out)
{
    double n = ldexp(1.0, g->bzoom);
    double mx = (g->place.ox + eye[0] / g->place.metres) * n;
    double my = (g->place.oy + eye[2] / g->place.metres) * n;
    int r = (int)g->bradius, cx = (int)floor(mx), cy = (int)floor(my);
    unsigned count = 0;

    for (int ring = 0; ring <= r; ring++) {
        for (int dy = -ring; dy <= ring; dy++) {
            for (int dx = -ring; dx <= ring; dx++) {
                int x = cx + dx, y = cy + dy;

                if ((abs(dx) != ring && abs(dy) != ring) || x < 0 || y < 0 || x >= (int)n ||
                    y >= (int)n) {
                    continue;
                }
                out[count++] = (struct k3d_tile){ (uint32_t)x, (uint32_t)y, (uint8_t)g->bzoom };
            }
        }
    }
    return count;
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
    /* The vector tiles about the eye's, nearest first, whose bytes have
     * not come: { "v", z, x, y }. */
    if (g->bzoom > 0) {
        struct k3d_tile around[81];
        unsigned na = buildings_about(g, eye, around);

        for (unsigned k = 0; k < na; k++) {
            struct btile *b = btile_of(g, around[k]);

            if (b == NULL || b->bytes == NULL) {
                push_tile(L, "v", around[k], &listed);
            } else if (g->rzoom > 0 && (b->state == MESHED || b->state == READY) &&
                       b->made > 0 && b->pieces != roof_all(g)) {
                /* Its roofs' photograph, the pieces not yet here (B4). */
                unsigned d = g->rzoom - g->bzoom, k2 = 1u << d;

                for (unsigned p = 0; p < k2 * k2; p++) {
                    if (!(b->pieces & (1u << p))) {
                        struct k3d_tile r = { (b->at.x << d) + p % k2, (b->at.y << d) + p / k2,
                                              (uint8_t)g->rzoom };

                        push_tile(L, "r", r, &listed);
                    }
                }
            }
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
            lua_pushnumber(L, k3d_flatten(g->flats, g->nflats, mx, my,
                                          map_heights_sample(&h->h, mx * n - t.x, my * n - t.y)));
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

/* g:buildings(z, x, y, path) -> true once the vector tile at `path` is
 * held, for the buildings over it - read here, gzipped or not, so a city's
 * megabyte never passes through Lua; false while there is no such file.
 * An empty file is a tile with none. */
static uint8_t *read_whole(const char *path, size_t *n, const char **why);

static int l_terrain_buildings(lua_State *L)
{
    struct terrain *g = terrain_check(L, 1);
    struct k3d_tile t = tile_args(L, 2);
    const char *path = luaL_checkstring(L, 5), *why = NULL;
    struct btile *b = btile_of(g, t);
    uint8_t *bytes;
    size_t n = 0;

    luaL_argcheck(L, g->bzoom > 0 && t.z == g->bzoom, 2, "a tile at the buildings' zoom");
    if (b != NULL && b->bytes != NULL) {
        lua_pushboolean(L, 1);
        return 1;
    }
    if ((bytes = read_whole(path, &n, &why)) == NULL) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (b == NULL) {
        if (!room((void **)&g->btiles, &g->btiles_room, g->nbtiles, sizeof(*g->btiles))) {
            free(bytes);
            return luaL_error(L, "out of memory for buildings");
        }
        b = &g->btiles[g->nbtiles++];
        memset(b, 0, sizeof(*b));
        b->at = t;
        b->state = WAITING;
        b->used = g->frame;
    }
    b->bytes = bytes;
    b->nbytes = n;
    lua_pushboolean(L, 1);
    return 1;
}

/* g:top(x, z, r) -> the highest roof of a building within `r` metres of
 * a place in the frame (x east, z south) each way, or nil where none
 * stands: what a camera put on an airfield must be above. */
static int l_terrain_top(lua_State *L)
{
    struct terrain *g = terrain_check(L, 1);
    float x = (float)luaL_checknumber(L, 2), z = (float)luaL_checknumber(L, 3);
    float r = (float)luaL_optnumber(L, 4, 0), top = -INFINITY;

    for (unsigned i = 0; i < g->nbtiles; i++) {
        const struct btile *b = &g->btiles[i];

        for (size_t k = 0; k < b->nboxes; k++) {
            const struct box *bx = &b->boxes[k];

            if (x + r >= bx->x0 && x - r <= bx->x1 && z + r >= bx->z0 && z - r <= bx->z1 &&
                bx->top > top) {
                top = bx->top;
            }
        }
    }
    if (!isfinite(top)) {
        return 0;
    }
    lua_pushnumber(L, top);
    return 1;
}

/* g:roof(z, x, y, surface): a piece of the photograph over a tile of
 * buildings, at the roofs' zoom - laid where it goes in the tile's roof
 * picture, scaled to ROOF_PIECE if it is not that already. With no
 * surface, the server had none: the piece is the walls' concrete, so the
 * picture is still whole - and not the sea an empty ground tile is. */
#define ROOF_CONCRETE 0xFF9E998Fu   /* 0.62, 0.60, 0.56: the walls' tint */
static int l_terrain_roof(lua_State *L)
{
    struct terrain *g = terrain_check(L, 1);
    struct k3d_tile t = tile_args(L, 2);
    unsigned w = 0, h = 0, pitch = 0;
    const uint32_t *px = lua_isnoneornil(L, 5) ? NULL : kosmos_surface_pixels(L, 5, &w, &h, &pitch);

    luaL_argcheck(L, g->rzoom > 0 && t.z == g->rzoom, 2, "a tile at the roofs' zoom");
    luaL_argcheck(L, px == NULL || (w >= 1 && h >= 1), 5, "a picture of a pixel at least");

    unsigned d = g->rzoom - g->bzoom, k = 1u << d, side = ROOF_PIECE * k;
    struct btile *b = btile_of(g, above(t, g->bzoom));
    unsigned px0 = t.x - (b ? b->at.x << d : 0), py0 = t.y - (b ? b->at.y << d : 0);

    if (b == NULL || (b->pieces & (1u << (py0 * k + px0)))) {
        return 0;
    }
    if (b->roof_pixels == NULL &&
        (b->roof_pixels = calloc((size_t)side * side, sizeof(*b->roof_pixels))) == NULL) {
        return luaL_error(L, "out of memory for a roof's picture");
    }
    for (unsigned y = 0; y < ROOF_PIECE; y++) {
        uint32_t *to = b->roof_pixels + (size_t)(py0 * ROOF_PIECE + y) * side + px0 * ROOF_PIECE;

        if (px == NULL) {
            for (unsigned x = 0; x < ROOF_PIECE; x++) {
                to[x] = ROOF_CONCRETE;
            }
            continue;
        }
        const uint32_t *row = (const uint32_t *)((const uint8_t *)px +
                                                 (size_t)(y * h / ROOF_PIECE) * pitch);

        for (unsigned x = 0; x < ROOF_PIECE; x++) {
            to[x] = row[x * w / ROOF_PIECE];
        }
    }
    b->pieces |= 1u << (py0 * k + px0);
    if (px != NULL) {
        b->photos |= 1u << (py0 * k + px0);
    }
    return 0;
}

/* g:built() -> "tiles=T ready=R buildings=B refused=F triangles=N": the
 * buildings held, as a line to print. */
static int l_terrain_built(lua_State *L)
{
    struct terrain *g = terrain_check(L, 1);
    unsigned ready = 0, made = 0, refused = 0, triangles = 0, roofed = 0, pieces = 0;

    for (unsigned i = 0; i < g->nbtiles; i++) {
        const struct btile *b = &g->btiles[i];

        pieces += (unsigned)__builtin_popcount(b->photos);
        if (b->state == READY) {
            ready++;
            made += b->made;
            refused += b->refused;
            triangles += b->triangles + b->roof_triangles;
            roofed += b->textured && b->photos != 0;
        }
    }
    lua_pushfstring(L, "tiles=%d ready=%d buildings=%d refused=%d triangles=%d roofed=%d "
                    "pieces=%d", (int)g->nbtiles, (int)ready, (int)made, (int)refused,
                    (int)triangles, (int)roofed, (int)pieces);
    return 1;
}

/* g:invent(kind, z, x, y): an invented island's tile, made here, as though
 * it had arrived - "e" its heights, as `elevation` keeps a tile's, "i" its
 * picture, as `imagery` does (`k3d_island.h`, `docs/flight.md` F5j). For a
 * window with nothing else to fly over: the network gone and the cache
 * empty. A tile at a time, a millisecond or so each, so the caller says
 * how many a frame. */
#define INVENTED_HEIGHTS 65
#define INVENTED_PICTURE 128
static int l_terrain_invent(lua_State *L)
{
    struct terrain *g = terrain_check(L, 1);
    const char *kind = luaL_checkstring(L, 2);
    struct k3d_tile t = tile_args(L, 3);

    if (kind[0] == 'e' && kind[1] == '\0') {
        if (heights_of(g, t) != NULL) {
            return 0;
        }
        if (!room((void **)&g->heights, &g->heights_room, g->nheights, sizeof(*g->heights))) {
            return luaL_error(L, "out of memory for heights");
        }
        struct heights *e = &g->heights[g->nheights];
        memset(e, 0, sizeof(*e));
        e->h.metres = malloc(sizeof(float) * INVENTED_HEIGHTS * INVENTED_HEIGHTS);
        if (e->h.metres == NULL) {
            return luaL_error(L, "out of memory for heights");
        }
        e->at = t;
        e->used = g->frame;
        e->h.width = e->h.height = INVENTED_HEIGHTS;
        k3d_island_heights(&g->place, t, INVENTED_HEIGHTS, e->h.metres, &e->h.lowest,
                           &e->h.highest);
        g->nheights++;
        return 0;
    }
    luaL_argcheck(L, kind[0] == 'i' && kind[1] == '\0', 2, "\"e\" or \"i\"");
    if (picture_of(g, t) != NULL) {
        return 0;
    }
    if (!room((void **)&g->pictures, &g->pictures_room, g->npictures, sizeof(*g->pictures))) {
        return luaL_error(L, "out of memory for imagery");
    }
    struct picture *p = &g->pictures[g->npictures];
    p->pixels = malloc(sizeof(uint32_t) * INVENTED_PICTURE * INVENTED_PICTURE);
    if (p->pixels == NULL) {
        return luaL_error(L, "out of memory for imagery");
    }
    p->at = t;
    p->width = p->height = INVENTED_PICTURE;
    k3d_island_picture(&g->place, t, INVENTED_PICTURE, p->pixels);
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
    struct k3d_vk_model_draw *models;   /* the frame's models' parts, `models_room` */
    unsigned models_room;
    char said[160];
    /* The keys, by keycode: which are down now, and which went down since
     * `pressed` last asked - a bit each, so reading them allocates
     * nothing a frame. */
    uint8_t held[KEYCODES / 8], went_down[KEYCODES / 8];
    /* What was typed since `typed` last asked, as UTF-8 - the characters
     * the window manager composed, with Return, Escape and Backspace as
     * theirs - and the last left press since `clicked` asked. A search
     * field's and a sheet's; a frame that typed nothing allocates nothing. */
    char typed[TYPED];
    unsigned typed_n;
    int clicked, click_x, click_y;
    int closing;
    /* The overlays `w:overlay` gave, on the device, in their places: a
     * picture each, refreshed in place while its size stays, made again
     * when it changes, and drawn in the places' order. */
    struct k3d_vk_texture overlay[OVERLAYS];
    struct k3d_vk_overlay laid[OVERLAYS];   /* texture NULL: none there */
    struct k3d_vk_overlay frame_laid[OVERLAYS];  /* the frame's, those there */
    uint32_t *packed;               /* a picture's rows without the pitch */
    size_t packed_room;             /* in pixels */
    /* What the last frame cost, for `w:stats`: the draws and triangles the
     * renderer recorded, and the device's own time for the frame. */
    uint32_t draws;
    uint64_t triangles;
    uint64_t shadow_triangles;      /* of them, into the sun's map */
    double gpu_ms;                  /* < 0: the device keeps no time */
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
            } else if (e.type == KW_KEY && e.key > 0 && e.key < 0x110000) {
                /* A character, as UTF-8; keys.lua's negative codes - the
                 * arrows, F1 - are not, and a held key is `held`'s. When
                 * nobody asks, the oldest are let go. */
                char u[4];
                unsigned n = 0, c = (unsigned)e.key;

                if (c < 0x80) {
                    u[n++] = (char)c;
                } else if (c < 0x800) {
                    u[n++] = (char)(0xc0 | (c >> 6));
                    u[n++] = (char)(0x80 | (c & 0x3f));
                } else if (c < 0x10000) {
                    u[n++] = (char)(0xe0 | (c >> 12));
                    u[n++] = (char)(0x80 | ((c >> 6) & 0x3f));
                    u[n++] = (char)(0x80 | (c & 0x3f));
                } else {
                    u[n++] = (char)(0xf0 | (c >> 18));
                    u[n++] = (char)(0x80 | ((c >> 12) & 0x3f));
                    u[n++] = (char)(0x80 | ((c >> 6) & 0x3f));
                    u[n++] = (char)(0x80 | (c & 0x3f));
                }
                if (w->typed_n + n > TYPED) {
                    w->typed_n = 0;
                }
                memcpy(w->typed + w->typed_n, u, n);
                w->typed_n += n;
            } else if (e.type == KW_POINTER && e.action == KW_PRESS && e.button == KW_LEFT) {
                w->clicked = 1;
                w->click_x = e.x;
                w->click_y = e.y;
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
    /* A tile's buildings are built again from the bytes it keeps. */
    for (unsigned i = 0; i < g->nbtiles; i++) {
        struct btile *b = &g->btiles[i];

        if (b->state == READY) {
            if (open && b->triangles > 0) {
                k3d_vk_model_free(&w->renderer, &b->model);
            }
            if (open && b->roof_triangles > 0) {
                k3d_vk_model_free(&w->renderer, &b->roofs);
            }
            b->state = WAITING;
            b->triangles = b->roof_triangles = 0;
        }
        if (b->textured) {
            if (open) {
                k3d_vk_texture_free(&w->renderer, &b->roof_texture);
            }
            b->textured = 0;
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
        for (int i = 0; i < OVERLAYS; i++) {
            k3d_vk_texture_free(&w->renderer, &w->overlay[i]);
        }
        k3d_vk_close(&w->renderer);
    }
    free(w->packed);
    kvk_swapchain_close(v, &w->chain);
    kvk_close(v);
    if (w->window != NULL) {
        kw_close(w->window);
    }
    free(w->drawn);
    free(w->models);
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
/*
 * w:overlay(surface, x, y, place) - a gfx surface laid over every frame
 * from now on at (x, y), by its alpha: a strip of instruments, a panel of
 * figures. `place` is 1 to 6, 1 when it is not given; the places are drawn
 * in order, so a higher one lies over a lower. Called again when the
 * picture changed, which copies it to the device again; with no surface,
 * that place is emptied. The pixels cross once, here, in C - never as a
 * Lua string - and a frame that changed nothing copies nothing.
 */
static int l_window_overlay(lua_State *L)
{
    struct window *w = window_check(L, 1);
    lua_Integer place = luaL_optinteger(L, 5, 1);
    unsigned width, height, pitch;
    uint32_t *pixels;

    luaL_argcheck(L, place >= 1 && place <= OVERLAYS, 5, "a place from 1 to 6");
    struct k3d_vk_texture *t = &w->overlay[place - 1];
    struct k3d_vk_overlay *laid = &w->laid[place - 1];

    if (lua_isnoneornil(L, 2)) {
        laid->texture = NULL;
        return 0;
    }
    pixels = kosmos_surface_pixels(L, 2, &width, &height, &pitch);
    if (width > w->renderer.width || height > w->renderer.height) {
        return luaL_error(L, "an overlay larger than the window");
    }
    if ((size_t)width * height > w->packed_room) {
        uint32_t *packed = realloc(w->packed, (size_t)width * height * 4);

        if (packed == NULL) {
            return luaL_error(L, "no memory for the overlay");
        }
        w->packed = packed;
        w->packed_room = (size_t)width * height;
    }
    for (unsigned row = 0; row < height; row++) {
        memcpy(w->packed + (size_t)row * width, (const uint8_t *)pixels + (size_t)row * pitch,
               (size_t)width * 4);
    }

    int done;

    if (t->width == width && t->height == height) {
        done = k3d_vk_texture_refresh(&w->renderer, t, w->packed);
    } else {
        k3d_vk_texture_free(&w->renderer, t);
        /* gfx's pixels are 0xAARRGGBB, which is B, G, R, A in memory. */
        done = k3d_vk_texture(&w->renderer, t, w->packed, width, height,
                              VK_FORMAT_B8G8R8A8_UNORM);
    }
    if (!done) {
        laid->texture = NULL;
        lua_pushnil(L);
        lua_pushstring(L, w->v.why[0] ? w->v.why : "overlay");
        w->v.why[0] = '\0';
        return 2;
    }
    *laid = (struct k3d_vk_overlay){ t, (int32_t)luaL_optinteger(L, 3, 0),
                                     (int32_t)luaL_optinteger(L, 4, 0), width, height };
    lua_pushboolean(L, 1);
    return 1;
}

/* w:stats() -> draws, triangles, gpu_ms, presented, shadow_triangles: what
 * the last frame recorded, the device's time for it (nil where the device
 * keeps none), how a frame reaches the screen, and of the triangles those
 * drawn into the sun's map. Numbers only, so a frame that asks
 * allocates nothing. */
static int l_window_stats(lua_State *L)
{
    struct window *w = window_check(L, 1);

    lua_pushinteger(L, (lua_Integer)w->draws);
    lua_pushinteger(L, (lua_Integer)w->triangles);
    if (w->gpu_ms >= 0) {
        lua_pushnumber(L, w->gpu_ms);
    } else {
        lua_pushnil(L);
    }
    /* VK_KOSMOS_surface: the swapchain's image copied into the window's
     * shared surface, which the window manager composes. */
    lua_pushliteral(L, "a copy to the window");
    /* Of the triangles, the ones drawn into the sun's map (*Shadows*). */
    lua_pushinteger(L, (lua_Integer)w->shadow_triangles);
    return 5;
}

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

/* w:pad(n) -> nil, or game controller n's lx, ly, rx, ry (-1 to 1, up and
 * right positive), lt, rt (0 to 1) and buttons (`pad_decode.h`'s bits: A
 * is 1, B 2), as the window manager last said - and only while this window
 * is in front (`docs/flight.md`, F7). Numbers rather than a table, so a
 * frame that asks makes nothing for the collector. */
static int l_window_pad(lua_State *L)
{
    struct window *w = window_check(L, 1);
    lua_Integer n = luaL_optinteger(L, 2, 1);
    struct kw_pad p;

    if (w->window == NULL || n < 1 || !kw_pad(w->window, (unsigned)(n - 1), &p)) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushnumber(L, p.lx);
    lua_pushnumber(L, p.ly);
    lua_pushnumber(L, p.rx);
    lua_pushnumber(L, p.ry);
    lua_pushnumber(L, p.lt);
    lua_pushnumber(L, p.rt);
    lua_pushinteger(L, (lua_Integer)p.buttons);
    return 7;
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

/* w:typed() -> what was typed since this last asked, as UTF-8 - Return
 * as "\r", Escape as "\27", Backspace as the window manager sends it -
 * or nil when nothing was, so asking every frame allocates nothing. */
static int l_window_typed(lua_State *L)
{
    struct window *w = window_check(L, 1);

    if (w->typed_n == 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, w->typed, w->typed_n);
    w->typed_n = 0;
    return 1;
}

/* w:clicked() -> x, y of the last left press in the picture since this
 * last asked, or nil: a sheet's buttons and choices. */
static int l_window_clicked(lua_State *L)
{
    struct window *w = window_check(L, 1);

    if (!w->clicked) {
        lua_pushnil(L);
        return 1;
    }
    w->clicked = 0;
    lua_pushinteger(L, w->click_x);
    lua_pushinteger(L, w->click_y);
    return 2;
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
                j->flats = g->flats;
                j->nflats = g->nflats;
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

/*
 * The buildings about the eye moved on, as the chunks are: a tile whose
 * bytes have come sent to the builder once the heights under it are here,
 * a built one put on the device while the frame has uploads to spend - its
 * roofs and walls one model - and the tiles past `bkeep` given back, the
 * least lately wanted first. How many are drawn, in `drawn`, at most 81.
 */
static int advance_buildings(struct window *w, struct terrain *g, const double eye[3],
                             unsigned *uploaded, struct btile **drawn, unsigned *ndrawn)
{
    struct k3d_tile around[81];
    unsigned na = buildings_about(g, eye, around);

    *ndrawn = 0;
    for (unsigned k = 0; k < na; k++) {
        struct btile *b = btile_of(g, around[k]);

        if (b == NULL) {
            continue;
        }
        b->used = g->frame;
        if (b->state == WAITING && b->bytes != NULL) {
            struct heights *h = heights_of(g, above(b->at, g->elevation));
            struct job *j;

            if (h == NULL || (j = calloc(1, sizeof(*j))) == NULL) {
                continue;
            }
            h->used = g->frame;
            j->buildings = 1;
            j->place = g->place;
            j->at = b->at;
            j->from = h->at;
            j->h = h->h;
            j->flats = g->flats;
            j->nflats = g->nflats;
            j->bytes = b->bytes;
            j->nbytes = b->nbytes;
            if (!send_job(g, j)) {
                free(j);
                continue;
            }
            h->busy++;
            b->job = j;
            b->state = BUILDING;
        } else if (b->state == MESHED && *uploaded < g->uploads) {
            struct job *j = b->job;
            struct k3d_extruded *e = &j->solid;

            _Static_assert(sizeof(struct k3d_vk_model_vertex) == sizeof(struct k3d_extrude_vertex),
                           "an extruded corner is a model's corner");
            /* The walls and the roofs apart: the roofs wear the photograph. */
            if (e->nwall_index > 0 && e->nwall < UINT32_MAX && e->nwall_index < UINT32_MAX) {
                if (!k3d_vk_model(&w->renderer, &b->model,
                                  (const struct k3d_vk_model_vertex *)e->wall, (uint32_t)e->nwall,
                                  e->wall_index, (uint32_t)e->nwall_index)) {
                    return 0;
                }
                b->triangles = (unsigned)(e->nwall_index / 3);
            }
            if (e->nroof_index > 0 && e->nroof < UINT32_MAX && e->nroof_index < UINT32_MAX) {
                if (!k3d_vk_model(&w->renderer, &b->roofs,
                                  (const struct k3d_vk_model_vertex *)e->roof, (uint32_t)e->nroof,
                                  e->roof_index, (uint32_t)e->nroof_index)) {
                    return 0;
                }
                b->roof_triangles = (unsigned)(e->nroof_index / 3);
            }
            job_free(j);
            b->job = NULL;
            b->state = READY;
            ++*uploaded;
        }
        /* The photograph, on the device once every piece has come. */
        if (b->state == READY && g->rzoom > 0 && !b->textured && b->roof_pixels != NULL &&
            b->pieces == roof_all(g) && *uploaded < g->uploads) {
            unsigned side = ROOF_PIECE << (g->rzoom - g->bzoom);

            if (!k3d_vk_texture(&w->renderer, &b->roof_texture, b->roof_pixels, side, side,
                                VK_FORMAT_B8G8R8A8_UNORM)) {
                return 0;
            }
            b->textured = 1;
            ++*uploaded;
        }
    }
    while (g->nbtiles > g->bkeep) {
        unsigned oldest = g->nbtiles;

        for (unsigned i = 0; i < g->nbtiles; i++) {
            struct btile *b = &g->btiles[i];

            if (b->used != g->frame && b->state != BUILDING &&
                (oldest == g->nbtiles || b->used < g->btiles[oldest].used)) {
                oldest = i;
            }
        }
        if (oldest == g->nbtiles) {
            break;
        }
        struct btile *b = &g->btiles[oldest];

        if (b->state == READY) {
            if (b->triangles > 0) {
                k3d_vk_model_free(&w->renderer, &b->model);
            }
            if (b->roof_triangles > 0) {
                k3d_vk_model_free(&w->renderer, &b->roofs);
            }
            if (b->textured) {
                k3d_vk_texture_free(&w->renderer, &b->roof_texture);
            }
        } else if (b->state == MESHED) {
            job_free(b->job);
        }
        free(b->bytes);
        free(b->boxes);
        free(b->roof_pixels);
        g->btiles[oldest] = g->btiles[--g->nbtiles];
    }
    /* What is drawn, once nothing more moves in the list. */
    for (unsigned k = 0; k < na; k++) {
        struct btile *b = btile_of(g, around[k]);

        if (b != NULL && b->state == READY && (b->triangles > 0 || b->roof_triangles > 0)) {
            drawn[(*ndrawn)++] = b;
        }
    }
    return 1;
}

/* w:draw(terrain, view) -> "drawn=D of N ready=R building=B fallback=F
 * held=H freed=X uploads=U ms=T chunks=z/x/y,...", a frame drawn and
 * presented, `chunks` the ground it drew; nil and why when it could not be. */
struct model;
static struct model *model_check(lua_State *L, int index);
static const struct k3d_vk_model_draw *model_draws(struct model *m, uint32_t *n,
                                                    const float eye[3]);

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
    /* The sun's colour and the sky's glow (F6a), when the view names them:
     * a white sun and no glow without. */
    lua_getfield(L, 3, "light");
    if (!lua_isnil(L, -1)) {
        lua_pop(L, 1);
        numbers(L, 3, "light", view.light, 3);
    } else {
        lua_pop(L, 1);
    }
    lua_getfield(L, 3, "glow");
    view.glow = lua_isnil(L, -1) ? 0.0f : (float)luaL_checknumber(L, -1);
    lua_pop(L, 1);
    /* Shadows, when the view names their box (`docs/flight.md`,
     * *Shadows*): `shadow_radius` metres around `shadow_centre`; none
     * without. */
    lua_getfield(L, 3, "shadow_radius");
    if (!lua_isnil(L, -1)) {
        view.shadow_radius = (float)luaL_checknumber(L, -1);
        lua_pop(L, 1);
        numbers(L, 3, "shadow_centre", view.shadow_centre, 3);
    } else {
        lua_pop(L, 1);
    }
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
    /* Which part of a frame failed is said before the step's own words: on
     * the M700 a hang read only "wait/-4", from an upload or the frame. */
    if (!advance(w, g, n, &uploaded)) {
        lua_pushnil(L);
        lua_pushfstring(L, "uploading the ground: %s", v->why);
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
    /* The aircraft, or whatever models the frame is given: one, or a list
     * of them - an aircraft and the runways a shape made (F8b). */
    const struct k3d_vk_model_draw *plane = NULL;
    uint32_t nplane = 0;
    float at[3] = { (float)view.eye[0], (float)view.eye[1], (float)view.eye[2] };

    if (lua_istable(L, 4)) {
        lua_Integer many = (lua_Integer)lua_rawlen(L, 4);

        for (lua_Integer i = 1; i <= many; i++) {
            uint32_t some = 0;
            const struct k3d_vk_model_draw *d;

            lua_rawgeti(L, 4, i);
            d = model_draws(model_check(L, -1), &some, at);
            lua_pop(L, 1);
            if (nplane + some > w->models_room) {
                unsigned room = (nplane + some) * 2;
                struct k3d_vk_model_draw *bigger = realloc(w->models, room * sizeof(*bigger));

                if (bigger == NULL) {
                    return luaL_error(L, "out of memory for a frame's models");
                }
                w->models = bigger;
                w->models_room = room;
            }
            memcpy(w->models + nplane, d, some * sizeof(*d));
            nplane += some;
        }
        plane = w->models;
    } else if (!lua_isnoneornil(L, 4)) {
        plane = model_draws(model_check(L, 4), &nplane, at);
    }
    /* The buildings about the eye (B3), after the models given: plain, a
     * light concrete lit as the aircraft is, where they stand. */
    if (g->bzoom > 0) {
        struct btile *standing[81];
        unsigned nstanding = 0, spent = uploaded;

        if (!advance_buildings(w, g, eye, &spent, standing, &nstanding)) {
            lua_pushnil(L);
            lua_pushfstring(L, "uploading the buildings: %s", v->why);
            v->why[0] = '\0';
            return 2;
        }
        if (nstanding > 0) {
            unsigned extra = 2 * nstanding, put = 0;

            if (nplane + extra > w->models_room) {
                unsigned more = (nplane + extra) * 2;
                struct k3d_vk_model_draw *bigger = malloc(more * sizeof(*bigger));

                if (bigger == NULL) {
                    return luaL_error(L, "out of memory for a frame's models");
                }
                if (nplane > 0) {
                    memcpy(bigger, plane, nplane * sizeof(*plane));
                }
                free(w->models);
                w->models = bigger;
                w->models_room = more;
            } else if (plane != w->models && nplane > 0) {
                memmove(w->models, plane, nplane * sizeof(*plane));
            }
            /* The walls a light concrete; the roofs the photograph where it
             * has come, the concrete until then. */
            for (unsigned i = 0; i < nstanding; i++) {
                struct btile *b = standing[i];

                for (int part = 0; part < 2; part++) {
                    struct k3d_vk_model_draw *d = &w->models[nplane + put];

                    if ((part == 0 && b->triangles == 0) || (part == 1 && b->roof_triangles == 0)) {
                        continue;
                    }
                    memset(d, 0, sizeof(*d));
                    d->model = part == 0 ? &b->model : &b->roofs;
                    if (part == 1 && b->textured) {
                        d->texture = &b->roof_texture;
                        d->tint[0] = d->tint[1] = d->tint[2] = 1.0f;
                    } else {
                        d->tint[0] = 0.62f, d->tint[1] = 0.60f, d->tint[2] = 0.56f;
                    }
                    d->tint[3] = 1.0f;
                    d->matrix[0] = d->matrix[5] = d->matrix[10] = d->matrix[15] = 1.0f;
                    put++;
                }
            }
            plane = w->models;
            nplane += put;
        }
    }
    uint32_t laid = 0;

    for (int i = 0; i < OVERLAYS; i++) {
        if (w->laid[i].texture != NULL) {
            w->frame_laid[laid++] = w->laid[i];
        }
    }
    if ((index = kvk_acquire(v, c)) < 0 || !kvk_begin(v) || !kvk_time_start(v) ||
        !k3d_vk_draw_scene(&w->renderer, c->images[index], c->views[index], &view, NULL, 0,
                           w->drawn, count, plane, nplane, w->frame_laid, laid)) {
        lua_pushnil(L);
        lua_pushfstring(L, "recording the frame: %s", v->why[0] ? v->why : "draw");
        v->why[0] = '\0';
        return 2;
    }
    kvk_barrier(v, c->images[index], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    kvk_time_end(v);
    if (!kvk_submit(v) || !kvk_present(v, c, (uint32_t)index)) {
        lua_pushnil(L);
        lua_pushfstring(L, "drawing the frame: %s", v->why);
        v->why[0] = '\0';
        return 2;
    }
    w->draws = w->renderer.draws;
    w->triangles = w->renderer.triangles + w->renderer.shadow_triangles;
    w->shadow_triangles = w->renderer.shadow_triangles;
    w->gpu_ms = kvk_time_ms(v);
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
    float *pull;                    /* a shape's, a part each; NULL: a file's */
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
    free(m->pull);
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

/*
 * w:shape{ part, ... } -> a model made of numbers rather than read from a
 * file (`docs/flight.md` F8b): a runway, its markings, anything an
 * application makes of a few triangles. A part is
 *
 *     { vertices = { x, y, z, ... }, indices = { 1, 2, 3, ... },
 *       colour = { r, g, b, a }, pull = 0.004 }
 *
 * its vertices in the model's frame - the world's, until `place` says
 * otherwise - its triangles by vertex, counted from one as Lua counts, its
 * colour linear and untextured, and `pull` the share of the distance to
 * the eye it is drawn nearer by (`model_draws`), so a surface lying on the
 * ground is in front of the ground rather than fighting it for each pixel.
 * Each vertex's normal is its triangles', summed. Made once: the numbers
 * are Lua's only until they are on the device.
 */
static int l_window_shape(lua_State *L)
{
    struct window *w = window_check(L, 1);
    const char *why = NULL;
    lua_Integer parts;
    struct model *m;

    luaL_checktype(L, 2, LUA_TTABLE);
    parts = (lua_Integer)lua_rawlen(L, 2);
    luaL_argcheck(L, parts >= 1 && parts <= 4096, 2, "one part to 4096");

    m = lua_newuserdatauv(L, sizeof(*m), 1);
    memset(m, 0, sizeof(*m));
    luaL_setmetatable(L, MODEL);
    lua_pushvalue(L, 1);
    lua_setiuservalue(L, -2, 1);
    m->owner = w;
    m->serial = w->serial;
    m->glb.parts = calloc((size_t)parts, sizeof(*m->glb.parts));
    m->parts = calloc((size_t)parts, sizeof(*m->parts));
    m->draws = calloc((size_t)parts, sizeof(*m->draws));
    m->pull = calloc((size_t)parts, sizeof(*m->pull));
    m->textures = calloc(1, sizeof(*m->textures));
    if (m->glb.parts == NULL || m->parts == NULL || m->draws == NULL || m->pull == NULL ||
        m->textures == NULL) {
        why = "no memory for the shape";
        goto refused;
    }
    for (lua_Integer i = 0; i < parts; i++) {
        struct k3d_glb_part *p = &m->glb.parts[i];
        lua_Integer nv, ni;

        m->glb.nparts = (uint32_t)i + 1;     /* what is freed if this one fails */
        p->texture = -1;
        p->movable = -1;
        p->material = -1;
        p->mesh = (int)i;
        for (int k = 0; k < 16; k++) {
            p->matrix[k] = (k % 5 == 0) ? 1.0f : 0.0f;
        }
        p->colour[0] = p->colour[1] = p->colour[2] = p->colour[3] = 1.0f;
        lua_rawgeti(L, 2, i + 1);
        luaL_checktype(L, -1, LUA_TTABLE);

        lua_getfield(L, -1, "colour");
        if (lua_istable(L, -1)) {
            for (int k = 0; k < 4; k++) {
                lua_rawgeti(L, -1, k + 1);
                if (!lua_isnil(L, -1)) {
                    p->colour[k] = (float)luaL_checknumber(L, -1);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "pull");
        m->pull[i] = lua_isnil(L, -1) ? 0.0f : (float)luaL_checknumber(L, -1);
        lua_pop(L, 1);
        if (m->pull[i] < 0.0f || m->pull[i] > 0.1f) {
            why = "a part's pull is a share of the distance, 0 to 0.1";
            lua_pop(L, 1);
            goto refused;
        }

        lua_getfield(L, -1, "vertices");
        luaL_checktype(L, -1, LUA_TTABLE);
        nv = (lua_Integer)lua_rawlen(L, -1);
        if (nv < 9 || nv % 3 != 0 || nv / 3 > 1 << 20) {
            why = "a part's vertices are x, y, z three at a time, at least three of them";
            lua_pop(L, 2);
            goto refused;
        }
        p->nvertices = (uint32_t)(nv / 3);
        p->vertices = calloc(p->nvertices, sizeof(*p->vertices));
        if (p->vertices == NULL) {
            why = "no memory for the shape";
            lua_pop(L, 2);
            goto refused;
        }
        for (lua_Integer k = 0; k < nv; k++) {
            lua_rawgeti(L, -1, k + 1);
            p->vertices[k / 3].position[k % 3] = (float)luaL_checknumber(L, -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);

        lua_getfield(L, -1, "indices");
        luaL_checktype(L, -1, LUA_TTABLE);
        ni = (lua_Integer)lua_rawlen(L, -1);
        if (ni < 3 || ni % 3 != 0 || ni > 3 << 20) {
            why = "a part's indices are triangles, three at a time";
            lua_pop(L, 2);
            goto refused;
        }
        p->nindices = (uint32_t)ni;
        p->indices = calloc(p->nindices, sizeof(*p->indices));
        if (p->indices == NULL) {
            why = "no memory for the shape";
            lua_pop(L, 2);
            goto refused;
        }
        for (lua_Integer k = 0; k < ni; k++) {
            lua_Integer at;

            lua_rawgeti(L, -1, k + 1);
            at = luaL_checkinteger(L, -1);
            lua_pop(L, 1);
            if (at < 1 || at > (lua_Integer)p->nvertices) {
                why = "a part's index names no vertex of it";
                lua_pop(L, 2);
                goto refused;
            }
            p->indices[k] = (uint32_t)(at - 1);
        }
        lua_pop(L, 2);              /* the indices, the part */

        /* Each vertex's normal: its triangles' faces, summed, each weighted
         * by its area as an unnormalised cross product is. */
        for (uint32_t t = 0; t < p->nindices; t += 3) {
            const float *a = p->vertices[p->indices[t]].position;
            const float *b = p->vertices[p->indices[t + 1]].position;
            const float *c = p->vertices[p->indices[t + 2]].position;
            float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
            float v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            float n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                           u[0] * v[1] - u[1] * v[0] };

            for (int k = 0; k < 3; k++) {
                for (int j = 0; j < 3; j++) {
                    p->vertices[p->indices[t + k]].normal[j] += n[j];
                }
            }
        }
        for (uint32_t k = 0; k < p->nvertices; k++) {
            float *n = p->vertices[k].normal;
            float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);

            if (len > 0.0f) {
                n[0] /= len, n[1] /= len, n[2] /= len;
            } else {
                n[0] = 0.0f, n[1] = 1.0f, n[2] = 0.0f;
            }
        }
        m->glb.triangles += p->nindices / 3;
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
static const struct k3d_vk_model_draw *model_draws(struct model *m, uint32_t *n,
                                                    const float eye[3])
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
        /* A shape's part pulled towards the eye by its share of the
         * distance (`w:shape`): the whole of it scaled about the eye, so
         * every point stays on the ray it was seen along and only its
         * depth comes nearer - a runway in front of the ground it lies on,
         * at any distance the depth buffer can tell apart. */
        if (m->pull != NULL && m->pull[i] > 0.0f) {
            float s = 1.0f - m->pull[i], pull[16] = { 0 };

            pull[0] = pull[5] = pull[10] = s;
            pull[15] = 1.0f;
            for (int k = 0; k < 3; k++) {
                pull[12 + k] = eye[k] * m->pull[i];
            }
            multiply(pull, d->matrix, d->matrix);
        }
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
        { "invent", l_terrain_invent },
        { "buildings", l_terrain_buildings },
        { "built", l_terrain_built },
        { "top", l_terrain_top },
        { "roof", l_terrain_roof },
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
        { "pad", l_window_pad },
        { "typed", l_window_typed },
        { "clicked", l_window_clicked },
        { "model", l_window_model },
        { "shape", l_window_shape },
        { "overlay", l_window_overlay },
        { "stats", l_window_stats },
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
