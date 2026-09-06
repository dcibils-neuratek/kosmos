/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_WEB_PAINT_H
#define KOSMOS_WEB_PAINT_H

#include <stddef.h>

#include "lua.h"

struct surface;

/*
 * A laid-out page: where every word, rule and bullet ended up.
 *
 * Opaque, and it outlives the document it came from - the runs hold copies
 * of the bytes rather than the tree's strings, so a browser can parse, lay
 * out, free the DOM and keep scrolling.
 */
struct web_page;

/* Lays `document` out at `width` pixels. NULL if there was no memory. */
struct web_page *web_page_layout(lua_State *L, void *document, int width);

/* How tall it came out. */
int  web_page_height(const struct web_page *p);

/* Paints it into a surface `height` pixels tall, clipping what falls past
 * the bottom - a page may be laid out taller than the caller can hold. */
void web_page_paint(const struct web_page *p, struct surface *s,
                    unsigned height);

/* The href of the link under a point in page coordinates, or NULL. The
 * bytes belong to the page and are not NUL-terminated. */
const char *web_page_link_at(const struct web_page *p, int x, int y,
                             size_t *len);

void web_page_free(struct web_page *p);

#endif /* KOSMOS_WEB_PAINT_H */
