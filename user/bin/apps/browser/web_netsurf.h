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

/* `web.setup(default_css [, quirks_css])` -> true: NetSurf's own default
 * stylesheets, once, before any document is laid out by it. */
int web_netsurf_setup(lua_State *L);

/* `web.zoom(percent)`: all of the page, at the zoom every page is laid out
 * at from now on (`roadmap.md` 6zz, zoom). */
int web_netsurf_zoom(lua_State *L);

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

/* The page's linked stylesheets still to fetch: the `k`th's number and
 * address, or 0; and one's text, fetched, made its sheet - before the first
 * layout, which makes the cascade (`roadmap.md` 6zz j4). */
size_t web_ns_sheets(struct web_ns_doc *d, size_t k, const char **url);
bool   web_ns_sheet(struct web_ns_doc *d, size_t n, const char *text,
                    size_t len);

/* The pictures the layout asked for, in page order: how many; the `k`th's
 * address, box and whether it has arrived; and one arrived - the surface on
 * top of the stack, and its natural size. */
size_t web_ns_objects(struct web_ns_doc *d);
size_t web_ns_imports(struct web_ns_doc *d, size_t k, const char **url);
bool   web_ns_import(struct web_ns_doc *d, size_t id, const char *text, size_t len);
bool   web_ns_object(struct web_ns_doc *d, size_t k, const char **url,
                     int *x, int *y, int *w, int *h, bool *background,
                     bool *arrived);
bool   web_ns_picture(struct web_ns_doc *d, lua_State *L, size_t k,
                      int width, int height);
void        web_ns_close(struct web_ns_doc *d, lua_State *L);

/*
 * Its forms (`roadmap.md` 6zz j6). A press on the page - what it did:
 * "field" when a text field took the caret, "toggled", "select" when a
 * select was pressed and its menu is the browser's to show, "sent" when a
 * form was sent, or NULL where there is no field. A key for the field with the
 * caret - whether it was taken. A form sent, taken: its address, and for a
 * POST its body and type. And what changed on the page since last asked.
 */
const char *web_ns_click(struct web_ns_doc *d, lua_State *L, int x, int y);
bool        web_ns_key(struct web_ns_doc *d, lua_State *L, int key);
int         web_ns_select(struct web_ns_doc *d, lua_State *L);
bool        web_ns_select_choose(struct web_ns_doc *d, lua_State *L, int i);
bool        web_ns_focused(struct web_ns_doc *d);
void        web_ns_blur(struct web_ns_doc *d, lua_State *L);
bool        web_ns_sent(struct web_ns_doc *d, char **url, char **body,
                        const char **type);
bool        web_ns_dirty(struct web_ns_doc *d, int *x, int *y, int *w,
                         int *h);

/*
 * What the last paint spent, by kind, in counter ticks and calls
 * (`roadmap.md` 6zz h): fills (rectangles and the straight lines that are
 * borders), text, pictures at their own size and pictures scaled, the
 * other shapes, the rest (clips), and the whole paint - whose remainder is
 * NetSurf walking its boxes.
 */
struct web_ns_cost {
    unsigned long ticks, calls;
};

/*
 * And what the last layout spent, apart (`roadmap.md` 6zz, the browser's
 * speed): `boxes`, the tree made from the document the first time - the
 * cascade, every element's style selected, and the boxes built - and
 * `layout`, the boxes placed at a width, every time.
 */
struct web_ns_costs {
    struct web_ns_cost whole, fills, text, pictures, scaled, shapes, other;
    struct web_ns_cost boxes, layout;
};

const struct web_ns_costs *web_ns_costs(struct web_ns_doc *d);

#endif
