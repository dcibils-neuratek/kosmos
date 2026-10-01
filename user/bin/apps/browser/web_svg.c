/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * SVG, drawn (`roadmap.md` 6zz j5).
 *
 * libsvgtiny reads an SVG - through libdom's XML parser, which is expat's -
 * into a list of shapes: paths of straight lines and cubic curves, each
 * filled, stroked or both in one colour. This file walks them into straight
 * edges, curves flattened into as many short lines as their size needs, and
 * `web_raster.c` turns the edges into pixels, four at a time. Its sum is a
 * winding number, which is what SVG calls `nonzero`, its default;
 * `evenodd` is not told apart, and libsvgtiny does not say which.
 *
 * A stroke is a quadrilateral per segment, the line widened by half its
 * width either side, all wound the same way so overlaps add rather than
 * cancel - which leaves a notch at a sharp corner, where a browser would
 * draw a join. Text inside an SVG is not drawn.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include <svgtiny.h>

#include "web_raster.h"
#include "web_svg.h"

/* The one door into a surface from outside `gfx.c`, as `gamesoft.c` has it. */
uint32_t *kosmos_surface_pixels(lua_State *L, int index,
                                unsigned *w, unsigned *h, unsigned *pitch);

/* What a shape is drawn through: a fill's edges as they are, a stroke's as
 * a quadrilateral round each, both scaled to the picture. */
struct pen {
    struct raster *r;
    float sx, sy;           /* the diagram's units to pixels */
    float half;             /* half a stroke's width, 0 for a fill */
};

static void segment(const struct pen *p, float x0, float y0, float x1,
                    float y1)
{
    float nx, ny, len;

    x0 *= p->sx; y0 *= p->sy; x1 *= p->sx; y1 *= p->sy;

    if (p->half <= 0.0f) {
        raster_edge(p->r, x0, y0, x1, y1);
        return;
    }

    len = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));

    if (!(len >= 1e-6f)) {          /* nothing, or not a number */
        return;
    }

    nx = -(y1 - y0) / len * p->half;
    ny = (x1 - x0) / len * p->half;

    /* The same way round every time, so overlapping segments add. */
    raster_edge(p->r, x0 + nx, y0 + ny, x1 + nx, y1 + ny);
    raster_edge(p->r, x1 + nx, y1 + ny, x1 - nx, y1 - ny);
    raster_edge(p->r, x1 - nx, y1 - ny, x0 - nx, y0 - ny);
    raster_edge(p->r, x0 - nx, y0 - ny, x0 + nx, y0 + ny);
}

/*
 * A shape's path, walked: moves, lines, cubic curves flattened by their
 * size, and a close back to where the subpath began. A fill's subpaths are
 * closed whether the path says so or not, since an open fill is filled as
 * though it were.
 */
static void walk(const struct pen *p, const float *path, unsigned n,
                 bool fill)
{
    float sx = 0, sy = 0, cx = 0, cy = 0;
    unsigned i = 0;
    bool open = false;

    while (i < n) {
        int op = (int)path[i];

        if (op == svgtiny_PATH_MOVE && i + 2 < n) {
            if (fill && open) {
                segment(p, cx, cy, sx, sy);
            }

            sx = cx = path[i + 1];
            sy = cy = path[i + 2];
            open = true;
            i += 3;
        } else if (op == svgtiny_PATH_LINE && i + 2 < n) {
            segment(p, cx, cy, path[i + 1], path[i + 2]);
            cx = path[i + 1];
            cy = path[i + 2];
            i += 3;
        } else if (op == svgtiny_PATH_BEZIER && i + 6 < n) {
            float x1 = path[i + 1], y1 = path[i + 2];
            float x2 = path[i + 3], y2 = path[i + 4];
            float x3 = path[i + 5], y3 = path[i + 6];
            float reach = (fabsf(x1 - cx) + fabsf(x2 - x1) + fabsf(x3 - x2))
                          * p->sx
                          + (fabsf(y1 - cy) + fabsf(y2 - y1) + fabsf(y3 - y2))
                          * p->sy;
            /* A segment every three pixels, and no more than 128 however
             * large - or however not a number - the curve says it is. */
            int steps = reach < 381.0f ? (int)(reach / 3.0f) + 1 : 128, k;
            float px = cx, py = cy;

            for (k = 1; k <= steps; k++) {
                float t = (float)k / (float)steps, u = 1.0f - t;
                float bx = u * u * u * cx + 3 * u * u * t * x1
                           + 3 * u * t * t * x2 + t * t * t * x3;
                float by = u * u * u * cy + 3 * u * u * t * y1
                           + 3 * u * t * t * y2 + t * t * t * y3;

                segment(p, px, py, bx, by);
                px = bx;
                py = by;
            }

            cx = x3;
            cy = y3;
            i += 7;
        } else if (op == svgtiny_PATH_CLOSE) {
            segment(p, cx, cy, sx, sy);
            cx = sx;
            cy = sy;
            open = false;
            i += 1;
        } else {
            break;          /* not a path libsvgtiny makes: stop, not guess */
        }
    }

    if (fill && open) {
        segment(p, cx, cy, sx, sy);
    }
}

/* A parsed SVG, kept so that its size can be asked before a surface is
 * made for it and it is drawn once, at that size. */
#define SVG_MT "kosmos.web.svg"

struct svg {
    struct svgtiny_diagram *diagram;
};

static struct svg *check_svg(lua_State *L)
{
    struct svg *s = luaL_checkudata(L, 1, SVG_MT);

    if (s->diagram == NULL) {
        luaL_error(L, "that SVG has been let go");
    }

    return s;
}

/*
 * `web.svg(bytes)` -> an SVG, or nil and why (`web_svg.h`).
 */
int web_svg(lua_State *L)
{
    size_t len = 0;
    const char *bytes = luaL_checklstring(L, 1, &len);
    struct svg *s = lua_newuserdatauv(L, sizeof(*s), 0);
    svgtiny_code code;

    memset(s, 0, sizeof(*s));
    luaL_setmetatable(L, SVG_MT);
    s->diagram = svgtiny_create();

    if (s->diagram == NULL) {
        return luaL_error(L, "no memory for an SVG");
    }

    /* 300 by 150 is CSS's size for a replaced element that says none. */
    code = svgtiny_parse(s->diagram, bytes, len, "", 300, 150);

    if (code != svgtiny_OK || s->diagram->width <= 0
            || s->diagram->height <= 0) {
        svgtiny_free(s->diagram);
        s->diagram = NULL;
        lua_pushnil(L);
        lua_pushstring(L, code == svgtiny_NOT_SVG ? "that is not an SVG"
                          : code == svgtiny_OUT_OF_MEMORY ? "no memory for it"
                          : code == svgtiny_OK ? "the SVG has no size"
                          : "the SVG would not parse");
        return 2;
    }

    return 1;
}

/* `svg:size()` -> its own width and height, in CSS pixels. */
static int l_size(lua_State *L)
{
    struct svg *s = check_svg(L);

    lua_pushinteger(L, s->diagram->width);
    lua_pushinteger(L, s->diagram->height);
    return 2;
}

/*
 * `svg:draw(surface)`: the picture, scaled to fill the surface, over what
 * the surface holds - a surface is born clear, so a new one ends holding
 * the picture with its transparency.
 */
static int l_draw(lua_State *L)
{
    struct svg *s = check_svg(L);
    unsigned w = 0, h = 0, pitch = 0, i;
    uint32_t *pixels = kosmos_surface_pixels(L, 2, &w, &h, &pitch);
    struct svgtiny_diagram *diagram = s->diagram;
    struct raster r;
    struct pen pen;

    if (!raster_open(&r, (int)w, (int)h)) {
        return luaL_error(L, "no memory to draw an SVG %dx%d", (int)w,
                          (int)h);
    }

    pen.r = &r;
    pen.sx = (float)w / (float)diagram->width;
    pen.sy = (float)h / (float)diagram->height;

    for (i = 0; i < diagram->shape_count; i++) {
        const struct svgtiny_shape *shape = &diagram->shape[i];

        if (shape->path == NULL) {
            continue;
        }

        if (shape->fill != svgtiny_TRANSPARENT) {
            pen.half = 0.0f;
            walk(&pen, shape->path, shape->path_length, true);
            raster_paint(&r, pixels, pitch, shape->fill, true);
        }

        if (shape->stroke != svgtiny_TRANSPARENT && shape->stroke_width > 0) {
            pen.half = 0.25f * (float)shape->stroke_width
                       * (pen.sx + pen.sy);
            walk(&pen, shape->path, shape->path_length, false);
            raster_paint(&r, pixels, pitch, shape->stroke, true);
        }
    }

    raster_close(&r);
    return 0;
}

static int l_gc(lua_State *L)
{
    struct svg *s = luaL_checkudata(L, 1, SVG_MT);

    if (s->diagram != NULL) {
        svgtiny_free(s->diagram);
        s->diagram = NULL;
    }

    return 0;
}

void web_svg_kit(lua_State *L)
{
    static const luaL_Reg methods[] = {
        { "size", l_size },
        { "draw", l_draw },
        { NULL, NULL }
    };

    luaL_newmetatable(L, SVG_MT);
    lua_pushcfunction(L, l_gc);
    lua_setfield(L, -2, "__gc");
    luaL_newlib(L, methods);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}
