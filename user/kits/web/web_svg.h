/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * SVG, drawn (`web_svg.c`, `roadmap.md` 6zz j5).
 */

#ifndef WEB_SVG_H
#define WEB_SVG_H

#include "lua.h"

/* `web.svg(bytes)` -> an SVG, read and kept, or nil and why. It answers
 * `svg:size()`, its own width and height, and `svg:draw(surface)`, which
 * draws it scaled to fill the surface. */
int web_svg(lua_State *L);

/* The SVG's methods, registered once with the kit. */
void web_svg_kit(lua_State *L);

#endif /* WEB_SVG_H */
