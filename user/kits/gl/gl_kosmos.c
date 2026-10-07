/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * /Kosmos/Kits/gl: TinyGL, drawing into a Kosmos surface.
 *
 * **A kit rather than a library, and the rule decides it rather than a
 * preference.** A software rasteriser is a loop over pixels, which is the
 * one thing the language split says never belongs in Lua, so it is C and
 * the profile is a formality.
 *
 * The division is the same one the whole system uses: **Lua decides what to
 * draw and where; the loop over pixels happens down here.** The demos are
 * TinyGL's own C, run unmodified (`gl_demos.c`), and Lua drives them: it
 * starts one with `gl.start`, and each frame is `gl.frame()` and `gl.blit`
 * - two calls - while seven thousand lines of C turn that into a picture.
 *
 * TinyGL renders into its own 32-bit buffer and this copies it into a
 * surface row by row. The copy is not free and it is not avoidable either:
 * a surface's pitch is aligned to 64 bytes and is almost never `width * 4`,
 * which is the same discipline `gfx.md` §19.3 insists on everywhere else.
 * Pointing TinyGL straight at the surface would work only for the widths
 * where the two happen to agree, which is the worst kind of working.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"       /* USER_HEAP_SIZE, for the budget below */
#include "lua.h"
#include "lauxlib.h"

#include "kits/gfx/gfx_draw.h"

#include <GL/gl.h>
#include <GL/ostinygl.h>

/* Weak: a C project's runtime is linked without the system's demos, whose
 * globals its own copy of a demo defines too (`Makefile`, `TCC_RUNTIME_OBJS`). */
void kosmos_gl_demos(lua_State *L) __attribute__((weak));

#define GL_CONTEXT_MT  "kosmos.gl.context"

struct glctx {
    ostgl_context_t *ctx;
    int              width;
    int              height;
};

static struct glctx *check_ctx(lua_State *L, int index)
{
    struct glctx *g = luaL_checkudata(L, index, GL_CONTEXT_MT);

    if (g->ctx == NULL) {
        luaL_error(L, "this GL context has been closed");
    }

    return g;
}

/*
 * `gl.context(width, height)` -> a context, made current.
 *
 * 32 bits deep, because that is what a Kosmos surface is and converting
 * between depths on every frame to save memory nobody is short of would be
 * paying twice.
 */
static int l_context(lua_State *L)
{
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    struct glctx *g;

    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        lua_pushnil(L);
        lua_pushstring(L, "a GL context that size is not one this machine has");
        return 2;
    }

    /*
     * Refused here rather than attempted, because TinyGL does not return
     * from this failure - it asserts, and an assert is a panic.
     *
     * `ostgl_create_context` allocates a colour buffer and a conversion
     * buffer at four bytes a pixel each, and `ZB_open` a depth buffer at
     * two: ten bytes a pixel before anything is drawn. A 460x380 window
     * wants 1.75 MB of a 2 MB heap that Lua is already living in, and the
     * process died with nothing on the serial line - which read exactly
     * like a channel that was closed, and cost an evening on that theory.
     *
     * Three quarters of the heap, and the fraction is measured rather than
     * chosen: 388x400 wants 1516 KB and runs, 460x380 wants 1707 KB and
     * dies. Half the heap was tried first and would have refused the size
     * that demonstrably works, which is the other way to be wrong about a
     * limit - and the more annoying one, because it looks like caution.
     */
    {
        unsigned long want = (unsigned long)w * (unsigned long)h * 10UL;
        unsigned long budget = (USER_HEAP_SIZE / 4UL) * 3UL;

        if (want > budget) {
            unsigned long fits = budget / 10UL;

            lua_pushnil(L);
            lua_pushfstring(L,
                "a %dx%d context wants %d KB and this process may spend %d; "
                "about %d pixels fit",
                w, h, (int)(want / 1024), (int)(budget / 1024), (int)fits);
            return 2;
        }
    }

    g = lua_newuserdatauv(L, sizeof(*g), 0);
    memset(g, 0, sizeof(*g));
    g->ctx = NULL;
    g->width = w;
    g->height = h;

    luaL_getmetatable(L, GL_CONTEXT_MT);
    lua_setmetatable(L, -2);

    g->ctx = ostgl_create_context(w, h, 32);

    if (g->ctx == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "no memory for a GL context");
        return 2;
    }

    ostgl_make_current(g->ctx);
    return 1;
}

static int l_close(lua_State *L)
{
    struct glctx *g = luaL_checkudata(L, 1, GL_CONTEXT_MT);

    if (g->ctx != NULL) {
        ostgl_delete_context(g->ctx);
        g->ctx = NULL;
    }

    return 0;
}

/*
 * `gl.blit(context, surface [, x, y])` - the rendered frame onto a surface.
 *
 * Row by row and clipped, because the two have different ideas about how far
 * apart their rows are and only one of them is allowed to be right about a
 * Kosmos surface. The surface through `gfx`'s door, which refuses one that
 * was freed or a view of one, and hands back the pitch with the pixels.
 */
static int l_blit(lua_State *L)
{
    struct glctx *g = check_ctx(L, 1);
    unsigned sw = 0, sh = 0, pitch = 0;
    uint32_t *pixels = kosmos_surface_pixels(L, 2, &sw, &sh, &pitch);
    long dx = (long)luaL_optinteger(L, 3, 0);
    long dy = (long)luaL_optinteger(L, 4, 0);
    long from, to;
    const uint32_t *src;
    long y;

    src = (const uint32_t *)ostgl_convert_framebuffer(g->ctx);

    if (src == NULL) {
        return 0;
    }

    /* The columns of the frame that land on the surface: clipped at both
     * edges, so a frame placed partly off the left is not written before
     * the row it belongs to. */
    from = dx < 0 ? -dx : 0;
    to = dx + g->width > (long)sw ? (long)sw - dx : g->width;

    if (from >= to) {
        return 0;
    }

    for (y = 0; y < g->height; y++) {
        long ty = dy + y;
        uint32_t *dst;

        if (ty < 0 || ty >= (long)sh) {
            continue;
        }

        dst = (uint32_t *)(void *)((uint8_t *)pixels + (size_t)ty * pitch);
        memcpy(dst + dx + from, src + (size_t)y * g->width + from,
               (size_t)(to - from) * sizeof(uint32_t));
    }

    return 0;
}

void kosmos_gl_kit(lua_State *L)
{
    static const luaL_Reg api[] = {
        { "context",       l_context },
        { "close",         l_close },
        { "blit",          l_blit },
        { NULL, NULL }
    };

    if (luaL_newmetatable(L, GL_CONTEXT_MT)) {
        lua_pushcfunction(L, l_close);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, l_close);
        lua_setfield(L, -2, "close");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
    }

    lua_pop(L, 1);

    luaL_newlib(L, api);

    /* TinyGL's own demos join the same table: `gl.demos()`, `gl.start(...)`,
     * `gl.frame()`. They are the reason the kit exists at all. */
    if (kosmos_gl_demos != NULL) {
        kosmos_gl_demos(L);
    }
}
