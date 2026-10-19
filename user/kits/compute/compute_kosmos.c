/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Compute Kit** (`docs/compute.md` C3): programs of their own on the
 * GPU's execution units, from Lua.
 *
 *   local compute = use("/Kosmos/Kits/compute")
 *   compute.available()               -- true, or nil and why
 *   local k, why = compute.kernel(text)        -- assembled here by eu.lua,
 *                                              -- checked by the driver;
 *                                              -- why is "line 7: ..."
 *   local b = compute.buffer(bytes)   -- bytes the program and the GPU share
 *   local p = compute.picture(w, h)   -- pixels, BGRA, and p:surface() over them
 *   b:write(offset, s)  b:read(offset, n)  b.size  b:free()
 *   local t, why = compute.run{ kernel = k, grid = { 64, 768 }, simd = 16,
 *                               buffers = { p, { b, "r32f" } },
 *                               constants = string.pack("<ff", x, y) }
 *   t:done()  t:wait(ms)  t:time()    -- the GPU's own nanoseconds for it
 *
 * **Control by message, data by shared memory**: a buffer is a region this
 * kit makes and maps, write-combining - so what the processor wrote is in
 * memory before a run and what the GPU wrote is read from memory after
 * (`compute.md`, *Coherence*) - and enters in the GPU's table; a run is one
 * message, `compute_core.c`'s, and the bytes never travel in it.
 *
 * **A run answers at once with a ticket**. One is in flight at a time: a
 * second, before the first is done, answers nil and "busy" for the program
 * to try again on its own clock rather than this waiting for it. A ticket
 * holds what its run uses, so nothing it reads is let go under it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include <kosmos.h>
#include <gpuproto.h>

#include "compute_core.h"
#include "compute_door.h"
#include "kits/kitenv.h"
#include "kits/gfx/gfx_draw.h"

#define KERNEL_MT "kosmos.compute.kernel"
#define BUFFER_MT "kosmos.compute.buffer"
#define TICKET_MT "kosmos.compute.ticket"

void kosmos_compute_kit(lua_State *L);

static int eu_ref = LUA_NOREF;
static unsigned long tick_hz;

struct kernel {
    uint32_t id;                /* the driver's, 0 once let go */
    long     gpu;
    uint32_t instructions;
};

struct buffer {
    uint32_t id;                /* the driver's buffer id, 0 once let go */
    long     gpu;
    long     region;            /* this process's hold on the pages */
    unsigned long at;           /* where they are mapped here */
    struct compute_shape shape;
    size_t   size;              /* what was asked for */
    bool     picture;
};

struct ticket {
    uint32_t fence;
    long     gpu;
};

static const char *GPU_WHY[] = {
    [GPU_ERR_NO_DEVICE] = "no Intel GPU this driver knows",
    [GPU_ERR_BAD_OP] = "the driver did not take the request",
    [GPU_ERR_NO_MEMORY] = "the GPU has no room for it",
    [GPU_ERR_GTT_BUSY] = "the GPU's table is full",
    [GPU_ERR_NO_BUFFER] = "a buffer the GPU does not have",
    [GPU_ERR_NO_WAKE] = "the GPU did not wake",
    [GPU_ERR_NO_RING] = "the render engine would not start",
    [GPU_ERR_NO_ANSWER] = "the GPU did not answer",
    [GPU_ERR_BUSY] = "busy",
    [GPU_ERR_KERNEL] = "the kernel was refused",
    [GPU_ERR_NOT_YOURS] = "another program's",
};

static const char *KERNEL_WHY[] = {
    [GPU_KERNEL_SIZE] = "it is empty, too large, or not whole instructions",
    [GPU_KERNEL_COMPACT] = "it is compacted, which the driver does not read",
    [GPU_KERNEL_DEBUG] = "it sets a breakpoint",
    [GPU_KERNEL_OPCODE] = "the instruction is not one a kernel may use",
    [GPU_KERNEL_INDIRECT] = "it addresses a register through a0",
    [GPU_KERNEL_REGISTER] = "it names a register a kernel may not use",
    [GPU_KERNEL_UNIT] = "it sends to a unit a kernel may not use",
    [GPU_KERNEL_MESSAGE] = "it sends a message or an address the unit may not be sent - only buffers 0 to 7",
    [GPU_KERNEL_DESC] = "its send's descriptor is in a register",
    [GPU_KERNEL_BRANCH] = "it branches outside the kernel, or through a register",
    [GPU_KERNEL_END] = "the last instruction does not end the thread",
    [GPU_KERNEL_EOT] = "a thread's end is predicated, or not from r112-r127 to the spawner",
};

static const char *gpu_why(uint32_t error)
{
    return error < sizeof GPU_WHY / sizeof GPU_WHY[0] && GPU_WHY[error] != NULL
         ? GPU_WHY[error] : "the GPU's driver refused";
}

static int fail(lua_State *L, const char *why, uint32_t code)
{
    lua_pushnil(L);
    lua_pushstring(L, why);
    lua_pushinteger(L, code);
    return 3;
}

/* `/Devices/gpu`, as the calling program holds it. */
static long gpu_cap(lua_State *L)
{
    return kit_capability(L, "/Devices/gpu");
}

/* One request: `len` bytes of `req` and a capability to pass, or -1. The
 * reply's words in `out`, its error answered; ~0u when nobody answered. */
static uint32_t ask(long gpu, const void *req, size_t len, long pass, struct gpu_reply *out)
{
    static struct message msg, rep;

    memset(&msg, 0, sizeof msg);
    memset(out, 0, sizeof *out);
    msg.length = (uint32_t)len;
    msg.cap_plus_one = pass >= 0 ? (uint32_t)pass + 1u : 0u;
    memcpy(msg.data, req, len);

    if (gpu < 0 || kosmos_call(gpu, &msg, &rep) != 0 || rep.length < sizeof *out) {
        out->error = ~0u;
        return ~0u;
    }

    memcpy(out, rep.data, sizeof *out);
    return out->error;
}

static uint32_t ask_simple(long gpu, uint32_t op, int32_t x, struct gpu_reply *out)
{
    struct gpu_request rq;

    memset(&rq, 0, sizeof rq);
    rq.op = op;
    rq.x = x;
    return ask(gpu, &rq, sizeof rq, -1, out);
}

/* compute.available(): whether there is a GPU to run on - the stats'
 * first word, which says no device where there is none. */
static int l_available(lua_State *L)
{
    struct gpu_reply rep;
    uint32_t e = ask_simple(gpu_cap(L), GPU_OP_STATS, 0, &rep);

    if (e != GPU_OK) {
        return fail(L, e == ~0u ? "no GPU's driver here" : gpu_why(e), e);
    }

    lua_pushboolean(L, 1);
    return 1;
}

/*--------------------------------------------------------------------------
 * Kernels.
 *------------------------------------------------------------------------*/

static void kernel_forget(struct kernel *k)
{
    struct gpu_reply rep;

    if (k->id != 0 && ask_simple(k->gpu, GPU_OP_KERNEL_FORGET, (int32_t)k->id, &rep) != GPU_ERR_BUSY) {
        k->id = 0;
    }
}

static int l_kernel_forget(lua_State *L)
{
    kernel_forget(luaL_checkudata(L, 1, KERNEL_MT));
    return 0;
}

static int l_kernel_index(lua_State *L)
{
    struct kernel *k = luaL_checkudata(L, 1, KERNEL_MT);
    const char *key = luaL_checkstring(L, 2);

    if (strcmp(key, "instructions") == 0) {
        lua_pushinteger(L, k->instructions);
    } else if (strcmp(key, "forget") == 0) {
        lua_pushcfunction(L, l_kernel_forget);
    } else {
        lua_pushnil(L);
    }

    return 1;
}

/*
 * compute.kernel(text): assembled by eu.lua, its bytes handed the driver
 * in a region, which checks them. Refused, "line N: why" by the lines
 * eu.lua said each instruction came from.
 */
static int l_kernel(lua_State *L)
{
    static char why[200];
    size_t len, pages;
    const char *code;
    struct gpu_request rq;
    struct gpu_reply rep;
    struct kernel *k;
    long gpu = gpu_cap(L), region, at;
    uint32_t e;

    luaL_checkstring(L, 1);

    if (!kit_library(L, &eu_ref, "/Kosmos/Libraries/eu.lua", why, sizeof why)) {
        return fail(L, why, 0);
    }

    lua_getfield(L, -1, "assemble");
    lua_pushvalue(L, 1);
    lua_call(L, 1, 2);                          /* code, lines - or nil, why */

    if (!lua_isstring(L, -2)) {
        return fail(L, lua_isstring(L, -1) ? lua_tostring(L, -1) : "the kernel did not assemble", 0);
    }

    code = lua_tolstring(L, -2, &len);
    pages = (len + 4095u) / 4096u;

    if ((region = kosmos_mem_create(pages)) < 0 || (at = kosmos_mem_map(region)) < 0) {
        if (region >= 0) {
            (void)kosmos_cap_drop(region);
        }

        return fail(L, "no memory for the kernel's bytes", 0);
    }

    memcpy((void *)at, code, len);

    memset(&rq, 0, sizeof rq);
    rq.op = GPU_OP_KERNEL;
    rq.x = (int32_t)len;
    e = ask(gpu, &rq, sizeof rq, region, &rep);

    (void)kosmos_share_unmap((unsigned long)at, pages);
    (void)kosmos_cap_drop(region);

    if (e == GPU_ERR_KERNEL) {
        uint32_t index = rep.words[1], kwhy = rep.words[2];
        long line = -1;

        lua_rawgeti(L, -1, (lua_Integer)index + 1);    /* lines[index + 1] */

        if (lua_isinteger(L, -1)) {
            line = (long)lua_tointeger(L, -1);
        }

        lua_pushfstring(L, line >= 0 ? "line %d: %s" : "instruction %d: %s",
                        line >= 0 ? (int)line : (int)index,
                        kwhy < sizeof KERNEL_WHY / sizeof KERNEL_WHY[0] && KERNEL_WHY[kwhy] != NULL
                        ? KERNEL_WHY[kwhy] : "refused");
        lua_pushnil(L);
        lua_insert(L, -2);
        lua_pushinteger(L, e);
        return 3;
    }

    if (e != GPU_OK) {
        return fail(L, e == ~0u ? "no GPU's driver here" : gpu_why(e), e);
    }

    k = lua_newuserdatauv(L, sizeof *k, 0);
    k->id = rep.buffer.id;
    k->gpu = gpu;
    k->instructions = (uint32_t)(len / 16u);
    luaL_setmetatable(L, KERNEL_MT);
    return 1;
}

/*--------------------------------------------------------------------------
 * Buffers.
 *------------------------------------------------------------------------*/

static void buffer_free(struct buffer *b)
{
    struct gpu_reply rep;

    if (b->id != 0 && ask_simple(b->gpu, GPU_OP_BUFFER_FORGET, (int32_t)b->id, &rep) != GPU_ERR_BUSY) {
        b->id = 0;
    }

    if (b->id == 0 && b->region >= 0) {
        (void)kosmos_share_unmap(b->at, b->shape.pages);
        (void)kosmos_cap_drop(b->region);
        b->region = -1;
        b->at = 0;
    }
}

static struct buffer *live(lua_State *L, int index)
{
    struct buffer *b = luaL_checkudata(L, index, BUFFER_MT);

    if (b->region < 0) {
        luaL_error(L, "a compute buffer used after it was freed");
    }

    return b;
}

void *compute_buffer_bytes(lua_State *L, int index, size_t *size)
{
    struct buffer *b = live(L, index);

    *size = b->size;
    return (void *)b->at;
}

static int l_buffer_free(lua_State *L)
{
    buffer_free(luaL_checkudata(L, 1, BUFFER_MT));
    return 0;
}

/* b:write(offset, s): bytes in, held to the buffer's size. */
static int l_buffer_write(lua_State *L)
{
    struct buffer *b = live(L, 1);
    lua_Integer offset = luaL_checkinteger(L, 2);
    size_t len;
    const char *s = luaL_checklstring(L, 3, &len);

    luaL_argcheck(L, offset >= 0 && (size_t)offset <= b->size && len <= b->size - (size_t)offset, 2,
                  "past the buffer's end");
    memcpy((char *)b->at + offset, s, len);
    return 0;
}

/* b:read(offset, n): bytes out, as a string. */
static int l_buffer_read(lua_State *L)
{
    struct buffer *b = live(L, 1);
    lua_Integer offset = luaL_checkinteger(L, 2);
    lua_Integer n = luaL_optinteger(L, 3, (lua_Integer)b->size - offset);

    luaL_argcheck(L, offset >= 0 && n >= 0 && (size_t)offset <= b->size
                  && (size_t)n <= b->size - (size_t)offset, 2, "past the buffer's end");
    lua_pushlstring(L, (const char *)b->at + offset, (size_t)n);
    return 1;
}

/*
 * p:surface(): the picture's pixels as a gfx surface, the same pages - a
 * window shows what the GPU wrote without a copy. The surface keeps the
 * buffer: a weak-keyed table holds the buffer for as long as the surface
 * lives, so its pages are never let go under it.
 */
static int l_buffer_surface(lua_State *L)
{
    struct buffer *b = live(L, 1);

    luaL_argcheck(L, b->picture, 1, "a buffer of bytes has no surface; compute.picture does");

    gfx_surface_new(L, (uint32_t *)b->at, b->shape.width, b->shape.height, b->shape.pitch, 0);

    if (luaL_getsubtable(L, LUA_REGISTRYINDEX, "kosmos.compute.keeps") == 0) {
        lua_pushstring(L, "k");
        lua_setfield(L, -2, "__mode");
        lua_pushvalue(L, -1);
        lua_setmetatable(L, -2);
    }

    lua_pushvalue(L, -2);                      /* the surface */
    lua_pushvalue(L, 1);                       /* the buffer */
    lua_rawset(L, -3);
    lua_pop(L, 1);
    return 1;
}

static int l_buffer_index(lua_State *L)
{
    struct buffer *b = luaL_checkudata(L, 1, BUFFER_MT);
    const char *key = luaL_checkstring(L, 2);

    if (strcmp(key, "size") == 0) {
        lua_pushinteger(L, (lua_Integer)b->size);
    } else if (strcmp(key, "width") == 0 && b->picture) {
        lua_pushinteger(L, b->shape.width);
    } else if (strcmp(key, "height") == 0 && b->picture) {
        lua_pushinteger(L, b->shape.height);
    } else if (strcmp(key, "pitch") == 0) {
        lua_pushinteger(L, b->shape.pitch);
    } else if (strcmp(key, "write") == 0) {
        lua_pushcfunction(L, l_buffer_write);
    } else if (strcmp(key, "read") == 0) {
        lua_pushcfunction(L, l_buffer_read);
    } else if (strcmp(key, "surface") == 0) {
        lua_pushcfunction(L, l_buffer_surface);
    } else if (strcmp(key, "free") == 0) {
        lua_pushcfunction(L, l_buffer_free);
    } else {
        lua_pushnil(L);
    }

    return 1;
}

/* A region of `shape`, mapped write-combining, entered in the GPU's table. */
static int make_buffer(lua_State *L, const struct compute_shape *shape, size_t size, bool picture)
{
    struct gpu_request rq;
    struct gpu_reply rep;
    struct buffer *b;
    long gpu = gpu_cap(L), region, at;
    uint32_t e;

    if ((region = kosmos_mem_create_flags(shape->pages, MEM_WRITE_COMBINE)) < 0) {
        return fail(L, "no memory for the buffer", 0);
    }

    if ((at = kosmos_mem_map(region)) < 0) {
        (void)kosmos_cap_drop(region);
        return fail(L, "the buffer's memory would not map", 0);
    }

    memset(&rq, 0, sizeof rq);
    rq.op = GPU_OP_BUFFER_IMPORT;
    rq.x = (int32_t)shape->width;
    rq.y = (int32_t)shape->height;
    rq.pitch = shape->pitch;
    e = ask(gpu, &rq, sizeof rq, region, &rep);

    if (e != GPU_OK) {
        (void)kosmos_share_unmap((unsigned long)at, shape->pages);
        (void)kosmos_cap_drop(region);
        return fail(L, e == ~0u ? "no GPU's driver here" : gpu_why(e), e);
    }

    b = lua_newuserdatauv(L, sizeof *b, 0);
    b->id = rep.buffer.id;
    b->gpu = gpu;
    b->region = region;
    b->at = (unsigned long)at;
    b->shape = *shape;
    b->size = size;
    b->picture = picture;
    luaL_setmetatable(L, BUFFER_MT);
    return 1;
}

static int l_buffer(lua_State *L)
{
    lua_Integer bytes = luaL_checkinteger(L, 1);
    struct compute_shape shape;
    const char *why = bytes > 0 ? compute_bytes_shape((size_t)bytes, &shape) : "a buffer of no bytes";

    if (why != NULL) {
        return fail(L, why, 0);
    }

    return make_buffer(L, &shape, (size_t)bytes, false);
}

static int l_picture(lua_State *L)
{
    lua_Integer w = luaL_checkinteger(L, 1), h = luaL_checkinteger(L, 2);
    struct compute_shape shape;
    const char *why = w > 0 && h > 0 && w <= 65535 && h <= 65535
                    ? compute_picture_shape((uint32_t)w, (uint32_t)h, &shape) : "a picture of no pixels";

    if (why != NULL) {
        return fail(L, why, 0);
    }

    return make_buffer(L, &shape, (size_t)shape.pitch * shape.height, true);
}

/*--------------------------------------------------------------------------
 * Runs and tickets.
 *------------------------------------------------------------------------*/

static uint32_t field_u32(lua_State *L, int table, const char *name, uint32_t fallback)
{
    lua_Integer v;

    lua_getfield(L, table, name);
    v = lua_isnil(L, -1) ? (lua_Integer)fallback : luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    return v < 0 || v > 0xffffffff ? 0u : (uint32_t)v;
}

/*
 * compute.run{ kernel, grid = { x, y }, simd, buffers, constants }: the
 * message built by `compute_pack_run`, and a ticket. Each buffer is a
 * compute buffer, or { buffer, view }; a picture is seen as BGRA and bytes
 * as bytes unless the view says otherwise.
 */
static int l_run(lua_State *L)
{
    static struct gpu_run q;
    struct compute_binding bind[GPU_RUN_BUFFERS];
    struct gpu_reply rep;
    struct kernel *k;
    struct ticket *t;
    const char *why, *constants = NULL;
    size_t nconst = 0;
    uint32_t gx = 1, gy = 1, n = 0, e;
    int i;

    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "kernel");
    k = luaL_testudata(L, -1, KERNEL_MT);
    lua_pop(L, 1);

    if (k == NULL || k->id == 0) {
        return fail(L, "a run needs a kernel, from compute.kernel", 0);
    }

    lua_getfield(L, 1, "grid");
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1);
        lua_rawgeti(L, -2, 2);
        gx = lua_isinteger(L, -2) ? (uint32_t)lua_tointeger(L, -2) : 0u;
        gy = lua_isnil(L, -1) ? 1u : lua_isinteger(L, -1) ? (uint32_t)lua_tointeger(L, -1) : 0u;
        lua_pop(L, 2);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "buffers");
    if (lua_istable(L, -1)) {
        int count = (int)lua_rawlen(L, -1);

        if (count > (int)GPU_RUN_BUFFERS) {
            return fail(L, "a run binds eight buffers at most", 0);
        }

        for (i = 1; i <= count; i++) {
            struct buffer *b;
            int view = -2;

            lua_rawgeti(L, -1, i);

            if (lua_istable(L, -1)) {
                lua_rawgeti(L, -1, 2);
                view = lua_isstring(L, -1) ? compute_view(lua_tostring(L, -1)) : -1;
                lua_pop(L, 1);
                lua_rawgeti(L, -1, 1);
                lua_replace(L, -2);
            }

            b = luaL_testudata(L, -1, BUFFER_MT);
            lua_pop(L, 1);

            if (b == NULL || b->id == 0) {
                return fail(L, "a run's buffers are compute buffers, freed none", 0);
            }

            if (view == -1) {
                return fail(L, "a view is \"bytes\", \"bgra\", \"r8\" or \"r32f\"", 0);
            }

            bind[n].id = b->id;
            bind[n].view = view >= 0 ? (uint32_t)view : b->picture ? GPU_VIEW_BGRA : GPU_VIEW_BYTES;
            n++;
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "constants");
    if (lua_isstring(L, -1)) {
        constants = lua_tolstring(L, -1, &nconst);
    }

    why = compute_pack_run(&q, k->id, field_u32(L, 1, "simd", 16), gx, gy, bind, n, constants, nconst);
    lua_pop(L, 1);

    if (why != NULL) {
        return fail(L, why, 0);
    }

    e = ask(k->gpu, &q, sizeof q, -1, &rep);

    if (e != GPU_OK) {
        return fail(L, e == ~0u ? "no GPU's driver here" : gpu_why(e), e);
    }

    /* The ticket, holding the run's table - its kernel and its buffers -
     * so none of them is collected while the GPU may be reading it. */
    t = lua_newuserdatauv(L, sizeof *t, 1);
    t->fence = rep.engine.value;
    t->gpu = k->gpu;
    lua_pushvalue(L, 1);
    lua_setiuservalue(L, -2, 1);
    luaL_setmetatable(L, TICKET_MT);
    return 1;
}

static bool ticket_done(struct ticket *t, bool *asked)
{
    struct gpu_reply rep;

    *asked = ask_simple(t->gpu, GPU_OP_FENCE, 1, &rep) == GPU_OK;
    return *asked && (int32_t)(rep.engine.value - t->fence) >= 0;
}

static int l_ticket_done(lua_State *L)
{
    struct ticket *t = luaL_checkudata(L, 1, TICKET_MT);
    bool asked;

    lua_pushboolean(L, ticket_done(t, &asked));
    return 1;
}

/* t:wait(ms): until the run is done, a scheduler tick at a time; true, or
 * nil and why. For a program that has nothing else to do meanwhile. */
static int l_ticket_wait(lua_State *L)
{
    struct ticket *t = luaL_checkudata(L, 1, TICKET_MT);
    lua_Integer ms = luaL_optinteger(L, 2, 1000);
    unsigned long ticks, i;
    bool asked;

    if (tick_hz == 0) {
        struct schedinfo info;

        tick_hz = kosmos_sched_info(&info) == 0 && info.tick_hz > 0 ? info.tick_hz : 250;
    }

    ticks = ms <= 0 ? 0 : (unsigned long)ms * tick_hz / 1000u + 1u;

    for (i = 0;; i++) {
        if (ticket_done(t, &asked)) {
            lua_pushboolean(L, 1);
            return 1;
        }

        if (!asked) {
            return fail(L, "the GPU's driver did not answer", 0);
        }

        if (i >= ticks) {
            break;
        }

        kosmos_sleep(1);
    }

    lua_pushnil(L);
    lua_pushfstring(L, "not done in %d ms", (int)ms);
    return 2;
}

/* t:time(): the GPU's own nanoseconds for the run, by its clock. */
static int l_ticket_time(lua_State *L)
{
    struct ticket *t = luaL_checkudata(L, 1, TICKET_MT);
    struct gpu_reply rep;
    uint32_t e = ask_simple(t->gpu, GPU_OP_RUN_TIME, (int32_t)t->fence, &rep);

    if (e != GPU_OK) {
        return fail(L, e == GPU_ERR_BUSY ? "not done yet"
                       : e == GPU_ERR_NO_ANSWER ? "too long ago: sixteen runs since" : gpu_why(e), e);
    }

    lua_pushinteger(L, (lua_Integer)(((uint64_t)rep.engine.head << 32) | rep.engine.value));
    return 1;
}

static int l_ticket_index(lua_State *L)
{
    struct ticket *t = luaL_checkudata(L, 1, TICKET_MT);
    const char *key = luaL_checkstring(L, 2);

    if (strcmp(key, "fence") == 0) {
        lua_pushinteger(L, t->fence);
    } else if (strcmp(key, "done") == 0) {
        lua_pushcfunction(L, l_ticket_done);
    } else if (strcmp(key, "wait") == 0) {
        lua_pushcfunction(L, l_ticket_wait);
    } else if (strcmp(key, "time") == 0) {
        lua_pushcfunction(L, l_ticket_time);
    } else {
        lua_pushnil(L);
    }

    return 1;
}

static int l_kernel_gc(lua_State *L)
{
    kernel_forget(luaL_checkudata(L, 1, KERNEL_MT));
    return 0;
}

static int l_buffer_gc(lua_State *L)
{
    buffer_free(luaL_checkudata(L, 1, BUFFER_MT));
    return 0;
}

void kosmos_compute_kit(lua_State *L)
{
    static const luaL_Reg api[] = {
        { "available", l_available },
        { "kernel",    l_kernel },
        { "buffer",    l_buffer },
        { "picture",   l_picture },
        { "run",       l_run },
        { NULL, NULL }
    };

    if (luaL_newmetatable(L, KERNEL_MT)) {
        lua_pushcfunction(L, l_kernel_index);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_kernel_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);

    if (luaL_newmetatable(L, BUFFER_MT)) {
        lua_pushcfunction(L, l_buffer_index);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_buffer_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);

    if (luaL_newmetatable(L, TICKET_MT)) {
        lua_pushcfunction(L, l_ticket_index);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    luaL_newlib(L, api);
    lua_pushinteger(L, GPU_RUN_CONSTANTS);
    lua_setfield(L, -2, "CONSTANTS");
    lua_pushinteger(L, GPU_RUN_BUFFERS);
    lua_setfield(L, -2, "BUFFERS");
}
