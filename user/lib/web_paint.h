/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_WEB_PAINT_H
#define KOSMOS_WEB_PAINT_H

#include "lua.h"

struct surface;

/* Lays the document out and paints it, returning the height it used.
 * `surface` NULL measures without drawing. */
int web_paint_document(lua_State *L, void *document, struct surface *surface,
                       int width, unsigned height);

#endif /* KOSMOS_WEB_PAINT_H */
