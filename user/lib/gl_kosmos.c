/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * /kits/gl: TinyGL, drawing into a Kosmos surface.
 *
 * **A kit rather than a library, and the rule decides it rather than a
 * preference.** `CLAUDE.md`: a finished algorithm has nothing to reload -
 * OpenGL 1.x is not going to change - so the usual price of C is not paid
 * and the profile is a formality. A software rasteriser is also exactly the
 * pixel loop the language split says never belongs in Lua.
 *
 * The division is the same one the whole system uses: **Lua decides what to
 * draw and where; the loop over pixels happens down here.** A demo says
 * `gl.rotate(a, 0, 1, 0)` and `gl.call_list(n)` once a frame - a handful of
 * calls - and seven thousand lines of C turn that into a picture.
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

#include "lua.h"
#include "lauxlib.h"

#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/ostinygl.h>

void kosmos_gl_demos(lua_State *L);

#define GL_CONTEXT_MT  "kosmos.gl.context"
#define SURFACE_MT     "kosmos.surface"

/* gfx.c's, and the one place outside it that needs to know the shape. It is
 * duplicated rather than shared because a header for one struct read by one
 * other file is more ceremony than the risk deserves - and the risk is
 * bounded: both are in this binary and a mismatch is a build away. */
struct surface {
    uint32_t *pixels;
    unsigned  width;
    unsigned  height;
    unsigned  pitch;
    size_t    bytes;
    size_t    pages;
    bool      owned;
};

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

    g = lua_newuserdatauv(L, sizeof(*g), 0);
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

static int l_make_current(lua_State *L)
{
    ostgl_make_current(check_ctx(L, 1)->ctx);
    return 0;
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
 * Kosmos surface.
 */
static int l_blit(lua_State *L)
{
    struct glctx *g = check_ctx(L, 1);
    struct surface *s = luaL_checkudata(L, 2, SURFACE_MT);
    long dx = (long)luaL_optinteger(L, 3, 0);
    long dy = (long)luaL_optinteger(L, 4, 0);
    const uint32_t *src;
    long y;

    if (s->pixels == NULL) {
        return luaL_error(L, "that surface has been freed");
    }

    src = (const uint32_t *)ostgl_convert_framebuffer(g->ctx);

    if (src == NULL) {
        return 0;
    }

    for (y = 0; y < g->height; y++) {
        long ty = dy + y;
        long width = g->width;
        uint32_t *dst;

        if (ty < 0 || ty >= (long)s->height) {
            continue;
        }

        if (dx + width > (long)s->width) {
            width = (long)s->width - dx;
        }

        if (width <= 0) {
            continue;
        }

        dst = (uint32_t *)(void *)((uint8_t *)s->pixels + (size_t)ty * s->pitch);
        memcpy(dst + dx, src + (size_t)y * g->width,
               (size_t)width * sizeof(uint32_t));
    }

    return 0;
}

/*--------------------------------------------------------------------------
 * The API itself.
 *
 * Enough of OpenGL 1.x for the demos TinyGL ships, and no more: what is here
 * was chosen by reading `gears.c` and `cube.c` rather than by working down
 * the specification. A call nobody makes is a call nobody has tested.
 *------------------------------------------------------------------------*/

#define GL0(name, call) \
    static int l_##name(lua_State *L) { (void)L; call; return 0; }

#define GL1F(name, call) \
    static int l_##name(lua_State *L) { \
        call((GLfloat)luaL_checknumber(L, 1)); return 0; }

#define GL3F(name, call) \
    static int l_##name(lua_State *L) { \
        call((GLfloat)luaL_checknumber(L, 1), (GLfloat)luaL_checknumber(L, 2), \
             (GLfloat)luaL_checknumber(L, 3)); return 0; }

#define GL4F(name, call) \
    static int l_##name(lua_State *L) { \
        call((GLfloat)luaL_checknumber(L, 1), (GLfloat)luaL_checknumber(L, 2), \
             (GLfloat)luaL_checknumber(L, 3), (GLfloat)luaL_checknumber(L, 4)); \
        return 0; }

GL0(load_identity,  glLoadIdentity())
GL0(push_matrix,    glPushMatrix())
GL0(pop_matrix,     glPopMatrix())
GL0(end_,           glEnd())
GL0(flush,          glFlush())

GL3F(translate, glTranslatef)
GL3F(normal,    glNormal3f)
GL3F(color3,    glColor3f)
GL4F(rotate,    glRotatef)
GL4F(color4,    glColor4f)
GL3F(scale,     glScalef)
GL3F(vertex3,   glVertex3f)

static int l_clear_color(lua_State *L)
{
    glClearColor((GLfloat)luaL_optnumber(L, 1, 0.0),
                 (GLfloat)luaL_optnumber(L, 2, 0.0),
                 (GLfloat)luaL_optnumber(L, 3, 0.0),
                 (GLfloat)luaL_optnumber(L, 4, 1.0));
    return 0;
}

static int l_clear(lua_State *L)
{
    /* Colour and depth together, which is what every frame wants and what
     * every demo asks for. A caller that wants one of them can say so. */
    int bits = (int)luaL_optinteger(L, 1, 0);

    glClear((bits != 0) ? bits
                        : (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    return 0;
}

static int l_viewport(lua_State *L)
{
    glViewport((GLint)luaL_checkinteger(L, 1), (GLint)luaL_checkinteger(L, 2),
               (GLint)luaL_checkinteger(L, 3), (GLint)luaL_checkinteger(L, 4));
    return 0;
}

static int l_matrix_mode(lua_State *L)
{
    const char *which = luaL_checkstring(L, 1);

    glMatrixMode((strcmp(which, "projection") == 0) ? GL_PROJECTION
                 : (strcmp(which, "texture") == 0) ? GL_TEXTURE
                 : GL_MODELVIEW);
    return 0;
}

static int l_frustum(lua_State *L)
{
    glFrustum(luaL_checknumber(L, 1), luaL_checknumber(L, 2),
              luaL_checknumber(L, 3), luaL_checknumber(L, 4),
              luaL_checknumber(L, 5), luaL_checknumber(L, 6));
    return 0;
}

static int l_perspective(lua_State *L)
{
    gluPerspective(luaL_checknumber(L, 1), luaL_checknumber(L, 2),
                   luaL_checknumber(L, 3), luaL_checknumber(L, 4));
    return 0;
}

static int l_begin(lua_State *L)
{
    static const struct { const char *name; GLenum mode; } modes[] = {
        { "points",         GL_POINTS },
        { "lines",          GL_LINES },
        { "line_loop",      GL_LINE_LOOP },
        { "line_strip",     GL_LINE_STRIP },
        { "triangles",      GL_TRIANGLES },
        { "triangle_strip", GL_TRIANGLE_STRIP },
        { "triangle_fan",   GL_TRIANGLE_FAN },
        { "quads",          GL_QUADS },
        { "quad_strip",     GL_QUAD_STRIP },
        { "polygon",        GL_POLYGON },
        { NULL, 0 }
    };

    const char *want = luaL_checkstring(L, 1);
    unsigned i;

    for (i = 0; modes[i].name != NULL; i++) {
        if (strcmp(modes[i].name, want) == 0) {
            glBegin(modes[i].mode);
            return 0;
        }
    }

    return luaL_error(L, "there is no primitive called %s", want);
}

/*
 * `enable` and `disable` by name, because a Lua caller should not be reciting
 * hexadecimal constants that a header already knows.
 */
static GLenum capability(lua_State *L, const char *want)
{
    static const struct { const char *name; GLenum bit; } caps[] = {
        { "depth_test",  GL_DEPTH_TEST },
        { "cull_face",   GL_CULL_FACE },
        { "lighting",    GL_LIGHTING },
        { "light0",      GL_LIGHT0 },
        { "light1",      GL_LIGHT1 },
        { "normalize",   GL_NORMALIZE },
        { "texture_2d",  GL_TEXTURE_2D },
        { "blend",       GL_BLEND },
        { "color_material", GL_COLOR_MATERIAL },
        { NULL, 0 }
    };

    unsigned i;

    for (i = 0; caps[i].name != NULL; i++) {
        if (strcmp(caps[i].name, want) == 0) {
            return caps[i].bit;
        }
    }

    luaL_error(L, "there is no capability called %s", want);
    return 0;
}

static int l_enable(lua_State *L)
{
    glEnable(capability(L, luaL_checkstring(L, 1)));
    return 0;
}

static int l_disable(lua_State *L)
{
    glDisable(capability(L, luaL_checkstring(L, 1)));
    return 0;
}

static int l_shade_model(lua_State *L)
{
    glShadeModel((strcmp(luaL_checkstring(L, 1), "flat") == 0)
                 ? GL_FLAT : GL_SMOOTH);
    return 0;
}

static int l_light(lua_State *L)
{
    GLfloat v[4];
    int which = (int)luaL_checkinteger(L, 1);
    const char *what = luaL_checkstring(L, 2);
    GLenum pname = (strcmp(what, "position") == 0) ? GL_POSITION
                   : (strcmp(what, "diffuse") == 0) ? GL_DIFFUSE
                   : (strcmp(what, "ambient") == 0) ? GL_AMBIENT
                   : GL_SPECULAR;

    v[0] = (GLfloat)luaL_optnumber(L, 3, 0.0);
    v[1] = (GLfloat)luaL_optnumber(L, 4, 0.0);
    v[2] = (GLfloat)luaL_optnumber(L, 5, 0.0);
    v[3] = (GLfloat)luaL_optnumber(L, 6, 1.0);

    glLightfv((GLenum)(GL_LIGHT0 + which), pname, v);
    return 0;
}

static int l_material(lua_State *L)
{
    GLfloat v[4];
    const char *what = luaL_checkstring(L, 1);
    GLenum pname = (strcmp(what, "ambient") == 0) ? GL_AMBIENT
                   : (strcmp(what, "specular") == 0) ? GL_SPECULAR
                   : (strcmp(what, "emission") == 0) ? GL_EMISSION
                   : GL_DIFFUSE;

    v[0] = (GLfloat)luaL_optnumber(L, 2, 0.0);
    v[1] = (GLfloat)luaL_optnumber(L, 3, 0.0);
    v[2] = (GLfloat)luaL_optnumber(L, 4, 0.0);
    v[3] = (GLfloat)luaL_optnumber(L, 5, 1.0);

    glMaterialfv(GL_FRONT_AND_BACK, pname, v);
    return 0;
}

/*
 * Display lists, which is what makes a demo cheap from Lua.
 *
 * The vertices of a gear are described once, in C's own terms, and every
 * frame afterwards is one `call_list`. Without them each frame would be
 * thousands of `vertex3` calls across the boundary, and that - not the
 * rasterising - would be what made it slow.
 */
static int l_new_list(lua_State *L)
{
    GLuint n = glGenLists(1);

    glNewList(n, GL_COMPILE);
    lua_pushinteger(L, (lua_Integer)n);
    return 1;
}

static int l_end_list(lua_State *L)
{
    (void)L;
    glEndList();
    return 0;
}

static int l_call_list(lua_State *L)
{
    glCallList((GLuint)luaL_checkinteger(L, 1));
    return 0;
}

void kosmos_gl_kit(lua_State *L)
{
    static const luaL_Reg api[] = {
        { "context",       l_context },
        { "make_current",  l_make_current },
        { "close",         l_close },
        { "blit",          l_blit },

        { "clear",         l_clear },
        { "clear_color",   l_clear_color },
        { "viewport",      l_viewport },
        { "matrix_mode",   l_matrix_mode },
        { "load_identity", l_load_identity },
        { "push_matrix",   l_push_matrix },
        { "pop_matrix",    l_pop_matrix },
        { "frustum",       l_frustum },
        { "perspective",   l_perspective },
        { "translate",     l_translate },
        { "rotate",        l_rotate },
        { "scale",         l_scale },

        { "begin",         l_begin },
        { "finish",        l_end_ },
        { "vertex",        l_vertex3 },
        { "normal",        l_normal },
        { "color",         l_color3 },
        { "color4",        l_color4 },

        { "enable",        l_enable },
        { "disable",       l_disable },
        { "shade_model",   l_shade_model },
        { "light",         l_light },
        { "material",      l_material },

        { "new_list",      l_new_list },
        { "end_list",      l_end_list },
        { "call_list",     l_call_list },

        { "flush",         l_flush },
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
    kosmos_gl_demos(L);
}
