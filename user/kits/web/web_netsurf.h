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

struct web_ns_doc *web_ns_open(void *document, const char *base,
                               const char *charset);
int         web_ns_layout(struct web_ns_doc *d, lua_State *L, int width,
                          int height);

/* How wide the last layout reached: its width, or more where a box overflows
 * it; for a reader that fits a page rather than scrolling it sideways. */
int         web_ns_wide(const struct web_ns_doc *d);
void        web_ns_paint(struct web_ns_doc *d, lua_State *L,
                         struct surface *s, int width, int height, long from,
                         const int *area);
const char *web_ns_link_at(struct web_ns_doc *d, int x, int y);
const char *web_ns_why(struct web_ns_doc *d);

/* The pictures the layout asked for, in page order: how many; the `k`th's
 * address, box and whether it has arrived; and one arrived - the surface on
 * top of the stack, and its natural size. */
size_t web_ns_objects(struct web_ns_doc *d);
bool   web_ns_object(struct web_ns_doc *d, size_t k, const char **url,
                     int *x, int *y, int *w, int *h, bool *background,
                     bool *arrived);
bool   web_ns_picture(struct web_ns_doc *d, lua_State *L, size_t k,
                      int width, int height);
void        web_ns_close(struct web_ns_doc *d, lua_State *L);

#endif
