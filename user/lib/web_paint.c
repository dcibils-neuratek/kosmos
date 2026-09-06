/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A document, painted.
 *
 * The first thing here that turns a tree into pixels. It is not CSS layout:
 * there is no box model, no floats, no `width`, and a face is chosen by tag
 * rather than by the cascade. What it does have is the shape the real thing
 * needs - blocks stacked down the page, text broken into lines at a
 * *measured* width, and every line placed on a baseline rather than on a top
 * edge - so the engine that replaces it changes how a box is chosen and not
 * how one is drawn.
 *
 * **One crossing for the whole page.** `doc:render(surface, width)` lays out
 * and paints inside C and returns the height it used. A call per box would
 * cost more than the drawing - `gfx.md` 19.11 puts a Lua/C crossing at about
 * two thousand pixels of work, and a page is hundreds of boxes and thousands
 * of glyphs. `docfont.c` took the same decision for a page of a PDF.
 *
 * The surface is the caller's and is scrolled by blitting out of it, which
 * is why this lays a page out *once* into something taller than the window
 * rather than re-laying it on every scroll.
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include <dom/dom.h>

#include "gfx_draw.h"
#include "web_paint.h"

/*
 * The faces a page needs, and there are six.
 *
 * `gfx` holds twelve and the desktop's four roles are the first of them, so
 * eight are free and six is comfortably inside that. Headings step down in
 * size and are bold; `pre` and `code` are the only monospace; italic exists
 * for `em` and is not yet reached, because that is inline work.
 */
enum {
    FACE_H1, FACE_H2, FACE_H3, FACE_BODY, FACE_MONO, FACE_ITALIC,
    FACE_COUNT
};

struct page {
    int face[FACE_COUNT];
    int width;              /* the content width, in pixels */
    int y;                  /* the pen, moving down the page */
    struct surface *s;      /* NULL to measure without drawing */
    unsigned height;        /* what the surface can hold */
};

/* 0xAARRGGBB, the same order `css_color` uses and `gfx` expects. */
#define INK        0xff101010u
#define PAPER      0xffffffffu
#define QUOTE_INK  0xff404040u
#define RULE       0xffc0c0c0u

static int face_for(const struct page *p, const char *tag, size_t len)
{
    if (len == 2 && tag[0] == 'h') {
        if (tag[1] == '1') return p->face[FACE_H1];
        if (tag[1] == '2') return p->face[FACE_H2];
        return p->face[FACE_H3];        /* h3..h6 share a size */
    }

    if ((len == 3 && memcmp(tag, "pre", 3) == 0)
        || (len == 4 && memcmp(tag, "code", 4) == 0)) {
        return p->face[FACE_MONO];
    }

    if (len == 10 && memcmp(tag, "blockquote", 10) == 0) {
        return p->face[FACE_ITALIC];
    }

    return p->face[FACE_BODY];
}

/*
 * One line of text, placed on its baseline.
 *
 * `gfx_draw_text` takes the *top* of the line, and the caller of this knows
 * only where the line begins. Ascent is what turns one into the other, and
 * it is per face: a heading and a paragraph on the same page do not share a
 * top edge, and stacking them as though they did is what makes a document
 * look subtly broken rather than obviously so.
 */
static void put_line(struct page *p, int face, int x, const char *str,
                     size_t len, uint32_t ink)
{
    if (p->s != NULL && p->y >= 0 && (unsigned)p->y < p->height) {
        gfx_draw_text(p->s, face, x, p->y, str, len, ink, NULL);
    }

    p->y += gfx_draw_height(face);
}

/*
 * A block of text, wrapped to the content width.
 *
 * Greedy, one word at a time, each word measured once. Measuring the whole
 * candidate line on every word would be quadratic in a paragraph's length;
 * adding one advance at a time is what a line breaker does, and it is the
 * same arithmetic the inline engine will need for a real inline box.
 */
static void put_text(struct page *p, int face, int indent,
                     const char *text, size_t len, uint32_t ink)
{
    long room = p->width - indent;
    long space = gfx_draw_measure(face, " ", 1);
    size_t at = 0;
    size_t start = 0, end = 0;      /* the line so far, in bytes */
    long taken = 0;
    bool any = false;

    while (at <= len) {
        size_t word, wlen;
        long w;

        while (at < len && (text[at] == ' ' || text[at] == '\n'
                            || text[at] == '\t' || text[at] == '\r')) {
            at++;
        }

        word = at;

        while (at < len && text[at] != ' ' && text[at] != '\n'
               && text[at] != '\t' && text[at] != '\r') {
            at++;
        }

        wlen = at - word;

        if (wlen == 0) {
            break;
        }

        w = gfx_draw_measure(face, text + word, wlen);

        if (!any) {
            start = word;
            end = at;
            taken = w;
            any = true;
        } else if (taken + space + w <= room) {
            end = at;
            taken += space + w;
        } else {
            put_line(p, face, indent, text + start, end - start, ink);
            start = word;
            end = at;
            taken = w;
        }
    }

    if (any) {
        put_line(p, face, indent, text + start, end - start, ink);
    }
}

static bool is_element(dom_node *node)
{
    dom_node_type type;

    return node != NULL
        && dom_node_get_node_type(node, &type) == DOM_NO_ERR
        && type == DOM_ELEMENT_NODE;
}

/*
 * The tags that carry a paragraph's worth of text - the *leaves*, so a
 * `div` around three of them does not paint the page and then each
 * paragraph again. The same set `blocks()` uses, and for the same reason.
 */
static bool is_block(const char *name, size_t len)
{
    static const char *tags[] = {
        "h1", "h2", "h3", "h4", "h5", "h6",
        "p", "li", "dt", "dd", "blockquote", "pre", "figcaption",
        NULL
    };
    unsigned i;

    for (i = 0; tags[i] != NULL; i++) {
        if (strlen(tags[i]) == len && memcmp(tags[i], name, len) == 0) {
            return true;
        }
    }

    return false;
}

static void walk(struct page *p, dom_node *node, int depth)
{
    dom_node *child = NULL;

    /* Bounded, because a browser is handed documents written to break it
     * and this process has a fixed stack with a guard page under it. */
    if (depth > 64 || dom_node_get_first_child(node, &child) != DOM_NO_ERR) {
        return;
    }

    while (child != NULL) {
        dom_node *next = NULL;

        if (is_element(child)) {
            dom_string *name = NULL;

            if (dom_node_get_node_name(child, &name) == DOM_NO_ERR
                && name != NULL) {
                dom_string *low = NULL;

                if (dom_string_tolower(name, true, &low) == DOM_NO_ERR
                    && low != NULL) {
                    const char *tag = dom_string_data(low);
                    size_t tlen = dom_string_byte_length(low);

                    if (is_block(tag, tlen)) {
                        dom_string *text = NULL;

                        if (dom_node_get_text_content(child, &text)
                            == DOM_NO_ERR && text != NULL) {
                            int face = face_for(p, tag, tlen);
                            int indent = 0;
                            uint32_t ink = INK;
                            bool heading = (tlen == 2 && tag[0] == 'h');

                            if (tlen == 2 && memcmp(tag, "li", 2) == 0) {
                                indent = 24;
                            } else if (tlen == 10
                                       && memcmp(tag, "blockquote", 10) == 0) {
                                indent = 32;
                                ink = QUOTE_INK;
                            }

                            /* Space above a heading, so it belongs to what
                             * follows it rather than to what came before. */
                            if (heading) {
                                p->y += gfx_draw_height(face) / 2;
                            }

                            put_text(p, face, indent, dom_string_data(text),
                                     dom_string_byte_length(text), ink);

                            /* And a rule under the biggest two, which is
                             * what a heading is for. */
                            if (heading && (tag[1] == '1' || tag[1] == '2')
                                && p->s != NULL
                                && p->y + 4 < (int)p->height) {
                                gfx_draw_fill(p->s, 0, p->y + 3,
                                              p->width, 1, RULE);
                            }

                            p->y += gfx_draw_height(p->face[FACE_BODY]) / 2;
                            dom_string_unref(text);
                        }
                    }

                    dom_string_unref(low);
                }

                dom_string_unref(name);
            }
        }

        walk(p, child, depth + 1);

        (void)dom_node_get_next_sibling(child, &next);
        dom_node_unref(child);
        child = next;
    }
}

int web_paint_document(lua_State *L, void *document, struct surface *surface,
                       int width, unsigned height)
{
    struct page p;
    static const struct { const char *font; int px; } wanted[FACE_COUNT] = {
        { "ibmplexsans-bold",   28 },   /* h1 */
        { "ibmplexsans-bold",   22 },   /* h2 */
        { "ibmplexsans-bold",   18 },   /* h3 and below */
        { "ibmplexsans",        16 },   /* body */
        { "ibmplexmono",        15 },   /* pre, code */
        { "ibmplexsans-italic", 16 },   /* blockquote */
    };
    unsigned i;

    memset(&p, 0, sizeof(p));

    p.width  = width;
    p.s      = surface;
    p.height = height;
    p.y      = 8;

    for (i = 0; i < FACE_COUNT; i++) {
        lua_getglobal(L, "gfx");
        lua_getfield(L, -1, "face");
        lua_pushstring(L, wanted[i].font);
        lua_pushinteger(L, wanted[i].px);

        if (lua_pcall(L, 2, 1, 0) != LUA_OK || !lua_isinteger(L, -1)) {
            /* No such face, or the pool is full. The body face is a worse
             * answer than the right one and a better one than nothing. */
            p.face[i] = 0;
            lua_pop(L, 2);
            continue;
        }

        p.face[i] = (int)lua_tointeger(L, -1);
        lua_pop(L, 2);
    }

    if (surface != NULL) {
        gfx_draw_fill(surface, 0, 0, width, (long)height, PAPER);
    }

    walk(&p, (dom_node *)document, 0);

    return p.y;
}
