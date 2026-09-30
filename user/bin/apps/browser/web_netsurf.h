/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What NetSurf's layout stands on, when the browser around it is Kosmos's
 * (`web_netsurf.c`, `roadmap.md` 6zz j).
 */

#ifndef WEB_NETSURF_H
#define WEB_NETSURF_H

#include "lua.h"

/* Runs the work NetSurf scheduled, to the end - a box tree built in the
 * slices NetSurf builds it in, one after another, with nothing between. */
void web_netsurf_run(void);

/* `web.join(base, href)` -> the address `href` names on the page at `base`,
 * by NetSurf's own URL code; nil and why when either is not one. */
int web_netsurf_join(lua_State *L);

/* `web.log()` -> NetSurf's last few kilobytes of log lines, emptied. */
int web_netsurf_log(lua_State *L);

/* `web.setup(default_css [, quirks_css])` -> true: NetSurf's own default
 * stylesheets, once, before any document is laid out by it. */
int web_netsurf_setup(lua_State *L);

/*
 * A document laid out and drawn by NetSurf (`roadmap.md` 6zz j3): opened
 * over a parsed DOM and the address it came from, laid out at a width -
 * the page's height back, or -1 - and drawn a band at a time.
 */
struct web_ns_doc;
struct surface;

struct web_ns_doc *web_ns_open(void *document, const char *base);
int         web_ns_layout(struct web_ns_doc *d, lua_State *L, int width,
                          int height);
void        web_ns_paint(struct web_ns_doc *d, lua_State *L,
                         struct surface *s, int width, int height, long from);
const char *web_ns_link_at(struct web_ns_doc *d, int x, int y);
const char *web_ns_why(struct web_ns_doc *d);
void        web_ns_close(struct web_ns_doc *d);

#endif
