/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * TinyGL's own demos, running unmodified.
 *
 * **They are C and they stay C**, which is the whole point. The first
 * attempt at this ported `gears.c` into Lua by hand - two hundred lines of
 * vertices retyped - and that was wrong twice over: it is eight demos of
 * transcription, each one a fresh chance to get a normal backwards, and it
 * abandons upstream so the next release cannot simply be dropped in.
 *
 * Doom is the precedent and it is exactly this shape: vendored C, a thin
 * platform layer, and a small Lua application that opens a window and pumps
 * it. `doom_kosmos.c` is to Doom what this file is to `gears.c`.
 *
 * The demos are written against `examples/ui.h`, which asks for five
 * functions from the program - `init`, `draw`, `idle`, `reshape`, `key` -
 * and offers two from the backend. So **one backend gives all eight of
 * them**, and the only thing standing in the way is that eight files each
 * define a function called `draw`.
 *
 * That is solved on the compile line rather than in the source:
 * `-Ddraw=gears_draw` and its four companions, which the Makefile passes per
 * demo. A `-D` is a build step somebody can read, and it leaves
 * `runtime/upstream/tinygl/` byte for byte as its authors released it -
 * which is the one thing the rule about vendored code forbids breaking.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include <GL/gl.h>
#include <GL/ostinygl.h>

#define DEMO(name)                          \
    void name##_init(void);                 \
    void name##_draw(void);                 \
    void name##_idle(void);                 \
    void name##_reshape(int w, int h);      \
    GLenum name##_key(int k);

DEMO(bounce)
DEMO(cube)
DEMO(gears)
DEMO(mech)
DEMO(morph3d)
DEMO(spin)
DEMO(teapot)
DEMO(texobj)

#define ENTRY(name, what) \
    { #name, name##_init, name##_draw, name##_idle, name##_reshape, \
      name##_key, what }

static const struct demo {
    const char *name;
    void      (*init)(void);
    void      (*draw)(void);
    void      (*idle)(void);
    void      (*reshape)(int, int);
    GLenum    (*key)(int);
    const char *what;
} demos[] = {
    ENTRY(gears,   "Brian Paul's gears, the oldest OpenGL demo there is"),
    ENTRY(cube,    "a textured cube"),
    ENTRY(teapot,  "the Utah teapot, lit"),
    ENTRY(spin,    "two spinning shapes"),
    ENTRY(bounce,  "a bouncing ball"),
    ENTRY(morph3d, "morphing platonic solids"),
    ENTRY(mech,    "a walking mech, and the largest of them"),
    ENTRY(texobj,  "texture objects"),
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

/*
 * What `ui.h` promises the demos, and what Kosmos gives them instead.
 *
 * `swap_buffers` does nothing: TinyGL has already rendered into its own
 * buffer by the time it is called, and the copy into a window's surface is
 * `gl.blit`, which the application does when it is ready rather than when
 * the demo says so. `ui_loop` is never called - a demo's `main` is renamed
 * away with everything else - but it has to exist, because that `main` still
 * refers to it and the linker does not know it will never run.
 */
void swap_buffers(void);
void swap_buffers(void) { }

int ui_loop(int argc, char **argv, const char *name);
int ui_loop(int argc, char **argv, const char *name)
{
    (void)argc; (void)argv; (void)name;
    return 0;
}

static const struct demo *current;

static const struct demo *find(const char *name)
{
    unsigned i;

    for (i = 0; demos[i].name != NULL; i++) {
        if (strcmp(demos[i].name, name) == 0) {
            return &demos[i];
        }
    }

    return NULL;
}

/* `gl.demos()` -> a list of { name, what }, for a launcher to show. */
static int l_demos(lua_State *L)
{
    unsigned i;

    lua_newtable(L);

    for (i = 0; demos[i].name != NULL; i++) {
        lua_newtable(L);
        lua_pushstring(L, demos[i].name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, demos[i].what);
        lua_setfield(L, -2, "what");
        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }

    return 1;
}

/*
 * `gl.start(name, width, height)` - begin a demo.
 *
 * `init` then `reshape`, which is the order `ui_loop` uses and the order the
 * demos assume: several of them build display lists in `init` and depend on
 * a projection that `reshape` sets.
 */
static int l_start(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    int w = (int)luaL_checkinteger(L, 2);
    int h = (int)luaL_checkinteger(L, 3);
    const struct demo *d = find(name);

    if (d == NULL) {
        lua_pushnil(L);
        lua_pushfstring(L, "there is no demo called %s", name);
        return 2;
    }

    current = d;
    d->init();
    d->reshape(w, h);

    lua_pushboolean(L, 1);
    return 1;
}

/* `gl.frame()` - one frame: move, then draw. */
static int l_frame(lua_State *L)
{
    if (current == NULL) {
        return luaL_error(L, "no demo has been started");
    }

    current->idle();
    current->draw();
    (void)L;
    return 0;
}

static int l_demo_key(lua_State *L)
{
    if (current != NULL) {
        current->key((int)luaL_checkinteger(L, 1));
    }

    return 0;
}

static int l_demo_reshape(lua_State *L)
{
    if (current != NULL) {
        current->reshape((int)luaL_checkinteger(L, 1),
                         (int)luaL_checkinteger(L, 2));
    }

    return 0;
}

void kosmos_gl_demos(lua_State *L)
{
    static const luaL_Reg api[] = {
        { "demos",   l_demos },
        { "start",   l_start },
        { "frame",   l_frame },
        { "key",     l_demo_key },
        { "reshape", l_demo_reshape },
        { NULL, NULL }
    };

    luaL_setfuncs(L, api, 0);
}
