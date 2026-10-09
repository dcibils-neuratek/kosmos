/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The web kit: HTML into a document, and CSS into a stylesheet.
 *
 * `use("/Kosmos/Kits/web")`, the same way `/Kosmos/Kits/pdf` and `/Kosmos/Kits/gl` are reached,
 * and for the same reason: which language a library is written in is not a
 * fact its caller should have to know.
 *
 * **It began as the smallest thing that is evidence.** Five libraries
 * compiling and linking says nothing about whether they run - a freestanding
 * libc can satisfy every symbol and still hand back a null on the first
 * allocation. So the first of this parsed a document and reported what was
 * in it, and parsed a stylesheet and reported whether it was understood.
 * Everything else the browser needs is built on top of those two answers:
 * selection, the `css_select_handler` bridging libdom's tree to libcss's
 * questions (`web_select.c`); the page laid out and drawn by NetSurf
 * (`web_netsurf.c`), or by `web_paint.c` where NetSurf could not; its
 * forms; and its SVGs (`web_svg.c`).
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include <dom/dom.h>
#include <dom/bindings/hubbub/parser.h>
#include <libcss/libcss.h>
#include <libcss/fpmath.h>
#include <libcss/unit.h>

#include "web_select.h"
#include "web_style.h"
#include "web_paint.h"
#include "web_netsurf.h"
#include "web_svg.h"
#include "kits/gfx/gfx_draw.h"

#define DOC_HANDLE  "kosmos.dom"

struct doc {
    dom_document *dom;      /* NULL once closed */

    /*
     * The last layout, kept.
     *
     * `render` is asked twice for every page - once with no surface, to
     * learn how tall the document is, and once with a surface that height -
     * and laying out twice would break every line twice for one picture.
     * The width is the key: a different one means a different set of line
     * breaks and nothing of the old layout survives.
     */
    struct web_page *page;
    int              page_width;

    /* The same document laid out by NetSurf (`roadmap.md` 6zz j3), made
     * the first time `ns_layout` is asked. */
    struct web_ns_doc *ns;

    /* The charset it was read in, which its forms are sent in too
     * (`roadmap.md` 6zz j7). */
    char charset[64];
};

static void forget_layout(struct doc *d)
{
    web_page_free(d->page);
    d->page = NULL;
    d->page_width = 0;
}

/*
 * A document is a userdata with a metatable rather than a number, which is
 * the same choice `net_kosmos.c` makes about a connection: a program that
 * was not handed one cannot name it.
 */
static struct doc *checkdoc(lua_State *L)
{
    struct doc *d = luaL_checkudata(L, 1, DOC_HANDLE);

    if (d->dom == NULL) {
        luaL_error(L, "this document has been closed");
    }

    return d;
}

/* Lua string -> dom_string, which every libdom lookup takes. */
static dom_string *to_dom(const char *s, size_t len)
{
    dom_string *out = NULL;

    if (dom_string_create((const uint8_t *)s, len, &out) != DOM_NO_ERR) {
        return NULL;
    }

    return out;
}

/*
 * Why the parser refused, in words - it said only "could not be parsed",
 * and Wikipedia's Dam article, cut short in transit, was refused with no
 * more than that (`roadmap.md` 6zz j). libdom's own failures, and hubbub's
 * carried inside `DOM_HUBBUB_HUBBUB_ERR`.
 */
static const char *parse_error(dom_hubbub_error e)
{
    switch (e) {
    case DOM_HUBBUB_NOMEM:
    case DOM_HUBBUB_HUBBUB_ERR_NOMEM:          return "out of memory";
    case DOM_HUBBUB_BADPARM:
    case DOM_HUBBUB_HUBBUB_ERR_BADPARM:        return "the parser was asked wrongly";
    case DOM_HUBBUB_DOM:                       return "building the document failed";
    case DOM_HUBBUB_HUBBUB_ERR_ENCODINGCHANGE: return "the page changed its encoding part way";
    case DOM_HUBBUB_HUBBUB_ERR_INVALID:        return "the HTML is invalid past repair";
    case DOM_HUBBUB_HUBBUB_ERR_NEEDDATA:       return "the page ends part way through";
    case DOM_HUBBUB_HUBBUB_ERR_BADENCODING:    return "the page's encoding is not one the parser knows";
    default:                                   return "an error the parser did not name";
    }
}

/*
 * **A document read as it arrives** (`roadmap.md` 6zz l1).
 *
 *   web.parser([charset]) -> p, or nil and why
 *   p:feed(bytes)         -> true, or nil and why once it has failed
 *   p:finish()            -> document, or nil and why
 *
 * hubbub takes a page in whatever pieces it comes in - a tag or a
 * character cut in two at a piece's end included - so the browser hands it
 * each piece as the network does, and the parse goes on while the rest is
 * still arriving rather than after the last byte. `web.parse` is the same
 * thing fed once.
 *
 * **`charset` is what the server said** in the page's `Content-Type`, and it
 * is held to: HTML ranks the header above anything the page says of itself
 * (WHATWG, "determining the character encoding"), so a `<meta>` cannot
 * change it. Google serves Latin America its page as ISO-8859-1 by the
 * header and UTF-8 by its `<meta>`, and the bytes are the header's - read
 * the other way, "Búsqueda" was "B?squeda" (`roadmap.md` 6zz j7). A name the
 * parser does not know is read as no name at all, and the page's own word
 * is taken instead.
 *
 * **And once more, when the page names its encoding part way.** Without a
 * charset from the header the parser starts on a guess and stops with
 * `ENCODINGCHANGE` when a `<meta charset>` says otherwise; it is then the
 * caller's to start again with that encoding stated, which NetSurf's
 * browser does - so Wikipedia's Dam article, arrived whole, was refused
 * until this did too (`roadmap.md` 6zz j). Fed in pieces, starting again
 * means every byte so far: so they are kept while a change is still
 * possible, and let go once the encoding is settled - from the start, when
 * the header said it. A second change is not asked about again.
 */
#define PARSING_HANDLE "kosmos.parsing"

struct parsing {
    dom_hubbub_parser        *parser;    /* NULL once finished or failed */
    dom_document             *document;
    dom_hubbub_parser_params  params;
    char                      charset[64];
    bool                      from_header;

    /* Every byte fed, while a `<meta>` may still change the encoding. */
    uint8_t                  *kept;
    size_t                    kept_len, kept_cap;

    const char               *failed;    /* why, once it has */
    int                       code;

    /* How many bytes were fed again when the encoding changed, or none. */
    size_t                    refed;
};

static void parsing_drop(struct parsing *p)
{
    if (p->parser != NULL) {
        dom_hubbub_parser_destroy(p->parser);
        p->parser = NULL;
    }

    if (p->document != NULL) {
        dom_node_unref((dom_node *)p->document);
        p->document = NULL;
    }

    free(p->kept);
    p->kept = NULL;
    p->kept_len = p->kept_cap = 0;
}

static bool parsing_start(struct parsing *p)
{
    while (dom_hubbub_parser_create(&p->params, &p->parser, &p->document)
           != DOM_HUBBUB_OK) {
        if (!p->from_header) {
            p->failed = "the parser could not be created";
            return false;
        }

        p->params.enc = NULL;       /* a name it does not know */
        p->from_header = false;
    }

    return true;
}

static bool parsing_begin(struct parsing *p, const char *said)
{
    memset(p, 0, sizeof(*p));

    p->params.enc           = NULL;  /* detect it, which is what a browser does */
    p->params.fix_enc       = true;
    p->params.enable_script = false; /* there is no interpreter to enable */

    if (said != NULL && said[0] != '\0' && strlen(said) < sizeof(p->charset)) {
        strcpy(p->charset, said);
        p->params.enc = p->charset;
        p->from_header = true;
    }

    return parsing_start(p);
}

static bool parsing_keep(struct parsing *p, const uint8_t *bytes, size_t len)
{
    if (p->kept_len + len > p->kept_cap) {
        size_t cap = p->kept_cap ? p->kept_cap : 65536;
        uint8_t *more;

        while (cap < p->kept_len + len) {
            cap *= 2;
        }

        more = realloc(p->kept, cap);

        if (more == NULL) {
            return false;
        }

        p->kept = more;
        p->kept_cap = cap;
    }

    memcpy(p->kept + p->kept_len, bytes, len);
    p->kept_len += len;

    return true;
}

static bool parsing_feed(struct parsing *p, const uint8_t *bytes, size_t len)
{
    dom_hubbub_error e;

    if (p->failed != NULL) {
        return false;
    }

    if (p->params.enc == NULL && !parsing_keep(p, bytes, len)) {
        p->failed = "out of memory";
        parsing_drop(p);
        return false;
    }

    e = dom_hubbub_parser_parse_chunk(p->parser, bytes, len);

    if (e == DOM_HUBBUB_HUBBUB_ERR_ENCODINGCHANGE && p->params.enc == NULL) {
        dom_hubbub_encoding_source source;
        const char *found = dom_hubbub_parser_get_encoding(p->parser, &source);

        if (found != NULL && strlen(found) < sizeof(p->charset)) {
            uint8_t *kept = p->kept;
            size_t kept_len = p->kept_len;

            strcpy(p->charset, found);
            p->kept = NULL;
            parsing_drop(p);
            p->params.enc = p->charset;

            if (!parsing_start(p)) {
                free(kept);
                return false;
            }

            e = dom_hubbub_parser_parse_chunk(p->parser, kept, kept_len);
            p->refed = kept_len;
            free(kept);
        }
    }

    /* Settled: nothing can ask to start again, so nothing is kept. */
    if (p->params.enc != NULL && p->kept != NULL) {
        free(p->kept);
        p->kept = NULL;
        p->kept_len = p->kept_cap = 0;
    }

    if (e != DOM_HUBBUB_OK) {
        p->failed = parse_error(e);
        p->code = (int)e;
        parsing_drop(p);
        return false;
    }

    return true;
}

static int parsing_said(lua_State *L, struct parsing *p)
{
    lua_pushnil(L);

    if (p->code != 0) {
        lua_pushfstring(L, "the document could not be parsed: %s (%d)",
                        p->failed, p->code);
    } else {
        lua_pushstring(L, p->failed);
    }

    return 2;
}

/* The parse ended, and its document as a `kosmos.dom`, or nil and why. */
static int parsing_finish(lua_State *L, struct parsing *p)
{
    dom_hubbub_encoding_source source;
    dom_document *document;
    char used[64];
    struct doc *d;

    if (p->failed != NULL) {
        return parsing_said(L, p);
    }

    if (p->parser == NULL) {
        lua_pushnil(L);
        lua_pushliteral(L, "this parse has finished already");
        return 2;
    }

    (void)dom_hubbub_parser_completed(p->parser);
    (void)snprintf(used, sizeof(used), "%s",
                   dom_hubbub_parser_get_encoding(p->parser, &source));
    dom_hubbub_parser_destroy(p->parser);
    p->parser = NULL;

    document = p->document;
    p->document = NULL;
    parsing_drop(p);

    if (document == NULL) {
        lua_pushnil(L);
        lua_pushliteral(L, "the parser produced no document");
        return 2;
    }

    /* Zeroed before anything reads it: `__gc` is `close`, and close frees a
     * layout. An uninitialised pointer there is a free of a random word. */
    d = lua_newuserdatauv(L, sizeof(*d), 0);
    memset(d, 0, sizeof(*d));
    d->dom = document;
    memcpy(d->charset, used, sizeof(d->charset));
    luaL_setmetatable(L, DOC_HANDLE);

    return 1;
}

static struct parsing *new_parsing(lua_State *L, const char *said)
{
    struct parsing *p = lua_newuserdatauv(L, sizeof(*p), 0);

    /* Begun before the metatable, so `__gc` never sees it half made. */
    (void)parsing_begin(p, said);
    luaL_setmetatable(L, PARSING_HANDLE);

    return p;
}

/* `web.parser([charset])` */
static int l_parser(lua_State *L)
{
    struct parsing *p = new_parsing(L, luaL_optstring(L, 1, NULL));

    if (p->failed != NULL) {
        return parsing_said(L, p);
    }

    return 1;
}

/* `p:feed(bytes)` */
static int l_parsing_feed(lua_State *L)
{
    struct parsing *p = luaL_checkudata(L, 1, PARSING_HANDLE);
    size_t len = 0;
    const char *bytes = luaL_checklstring(L, 2, &len);

    if (!parsing_feed(p, (const uint8_t *)bytes, len)) {
        return parsing_said(L, p);
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* `p:refed()` -> the bytes fed again when a `<meta>` changed the
 * encoding part way, or 0: what starting again cost. */
static int l_parsing_refed(lua_State *L)
{
    struct parsing *p = luaL_checkudata(L, 1, PARSING_HANDLE);

    lua_pushinteger(L, (lua_Integer)p->refed);
    return 1;
}

/* `p:finish()` */
static int l_parsing_finish(lua_State *L)
{
    return parsing_finish(L, luaL_checkudata(L, 1, PARSING_HANDLE));
}

static int l_parsing_gc(lua_State *L)
{
    parsing_drop(luaL_checkudata(L, 1, PARSING_HANDLE));
    return 0;
}

/*
 * parse(html [, charset]) -> document, or nil and why: the whole page fed
 * at once, for a page that is already whole - from the cache, the disk, or
 * this image.
 */
static int l_parse(lua_State *L)
{
    size_t len = 0;
    const char *html = luaL_checklstring(L, 1, &len);
    struct parsing *p = new_parsing(L, luaL_optstring(L, 2, NULL));

    if (p->failed != NULL || !parsing_feed(p, (const uint8_t *)html, len)) {
        return parsing_said(L, p);
    }

    return parsing_finish(L, p);
}

/* count(tag) -> how many elements have that name. */
static int l_count(lua_State *L)
{
    struct doc *d = checkdoc(L);
    size_t len = 0;
    const char *tag = luaL_checklstring(L, 2, &len);
    dom_string *name = to_dom(tag, len);
    dom_nodelist *list = NULL;
    uint32_t n = 0;

    if (name == NULL) {
        lua_pushnil(L);
        lua_pushliteral(L, "out of memory");
        return 2;
    }

    if (dom_document_get_elements_by_tag_name(d->dom, name, &list)
        != DOM_NO_ERR || list == NULL) {
        dom_string_unref(name);
        lua_pushnil(L);
        lua_pushliteral(L, "the document could not be searched");
        return 2;
    }

    (void)dom_nodelist_get_length(list, &n);

    dom_nodelist_unref(list);
    dom_string_unref(name);

    lua_pushinteger(L, (lua_Integer)n);
    return 1;
}

/*
 * The text of the first element with this name, or nil when there is none.
 *
 * `title()` is this with the name filled in, and both go through here so
 * there is one piece of tree-walking rather than two.
 */
static int text_of(lua_State *L, struct doc *d, const char *tag, size_t taglen)
{
    dom_string *name = to_dom(tag, taglen);
    dom_nodelist *list = NULL;
    dom_node *node = NULL;
    dom_string *text = NULL;
    uint32_t n = 0;

    if (name == NULL) {
        lua_pushnil(L);
        return 1;
    }

    if (dom_document_get_elements_by_tag_name(d->dom, name, &list)
        != DOM_NO_ERR || list == NULL) {
        dom_string_unref(name);
        lua_pushnil(L);
        return 1;
    }

    dom_string_unref(name);
    (void)dom_nodelist_get_length(list, &n);

    if (n == 0 || dom_nodelist_item(list, 0, &node) != DOM_NO_ERR
        || node == NULL) {
        dom_nodelist_unref(list);
        lua_pushnil(L);
        return 1;
    }

    if (dom_node_get_text_content(node, &text) == DOM_NO_ERR && text != NULL) {
        lua_pushlstring(L, dom_string_data(text), dom_string_byte_length(text));
        dom_string_unref(text);
    } else {
        lua_pushnil(L);
    }

    dom_node_unref(node);
    dom_nodelist_unref(list);

    return 1;
}

/*
 * An attribute of an element as a field of the table on the top of the
 * stack, when it has one - under the name it has in the markup.
 */
static void attribute_field(lua_State *L, dom_node *node, const char *attr,
                            const char *field)
{
    dom_string *name = to_dom(attr, strlen(attr));
    dom_string *value = NULL;

    if (name == NULL) {
        return;
    }

    if (dom_element_get_attribute((dom_element *)node, name, &value) == DOM_NO_ERR
        && value != NULL) {
        lua_pushlstring(L, dom_string_data(value), dom_string_byte_length(value));
        lua_setfield(L, -2, field);
        dom_string_unref(value);
    }

    dom_string_unref(name);
}

/*
 * `doc:meta()` -> every `<meta>` in the document, `{ http_equiv =, name =,
 * content = }` each, in order - wherever it is, a `<noscript>`'s included,
 * since no script runs here and a page without them means what is in one.
 * The browser reads a refresh out of it (`roadmap.md` 6zz, meta refresh):
 * DuckDuckGo's front page, to a browser that runs no scripts, is a hidden
 * body and a refresh to its page without them.
 */
static int l_meta(lua_State *L)
{
    struct doc *d = checkdoc(L);
    dom_string *tag = to_dom("meta", 4);
    dom_nodelist *list = NULL;
    uint32_t n = 0, i;

    lua_newtable(L);

    if (tag == NULL) {
        return 1;
    }

    if (dom_document_get_elements_by_tag_name(d->dom, tag, &list) != DOM_NO_ERR
        || list == NULL) {
        dom_string_unref(tag);
        return 1;
    }

    dom_string_unref(tag);
    (void)dom_nodelist_get_length(list, &n);

    for (i = 0; i < n; i++) {
        dom_node *node = NULL;

        if (dom_nodelist_item(list, i, &node) != DOM_NO_ERR || node == NULL) {
            continue;
        }

        lua_createtable(L, 0, 3);
        attribute_field(L, node, "http-equiv", "http_equiv");
        attribute_field(L, node, "name", "name");
        attribute_field(L, node, "content", "content");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
        dom_node_unref(node);
    }

    dom_nodelist_unref(list);
    return 1;
}

/*
 * `doc:lang()` -> the language the page says it is in, `<html lang>`, or nil:
 * which face draws its Han ideographs, Japanese, Korean or Chinese
 * (`gfx.font_prefer`, `roadmap.md` 6zz j5).
 */
static int l_lang(lua_State *L)
{
    struct doc *d = checkdoc(L);
    dom_node *root = NULL;

    lua_createtable(L, 0, 1);

    if (dom_document_get_document_element(d->dom, (void *)&root) == DOM_NO_ERR
        && root != NULL) {
        attribute_field(L, root, "lang", "lang");
        dom_node_unref(root);
    }

    lua_getfield(L, -1, "lang");
    return 1;
}

/*
 * text(tag) -> the text inside the first such element. The browser does not
 * ask it; `tools/run_web.py` does, to see an entity decoded.
 */
static int l_text(lua_State *L)
{
    struct doc *d = checkdoc(L);
    size_t len = 0;
    const char *tag = luaL_checklstring(L, 2, &len);

    return text_of(L, d, tag, len);
}

static int l_title(lua_State *L)
{
    return text_of(L, checkdoc(L), "title", 5);
}

/*
 * render(surface, width [, height [, from]]) -> the page's whole height; with
 * `from`, the band of it starting that far down, painted into the surface.
 *
 * The whole page in one crossing: layout and painting both happen in C and
 * what comes back is a number. A call per box would cost more than the
 * drawing, which is the same reason `docfont.c` takes a page of glyphs at
 * once rather than one at a time.
 *
 * **A nil surface measures**, and the caller needs that before it can do
 * anything else: a page is laid out once into a surface as tall as the
 * whole document and scrolled by blitting out of it, so the height has to
 * be known before the surface can be asked for.
 *
 * The layout is kept, so the second call paints the boxes the first one
 * made rather than making them again - and `link_at` has something to
 * answer from once the painting is done.
 */
static int l_render(lua_State *L)
{
    struct doc *d = checkdoc(L);
    struct surface *s = lua_isnoneornil(L, 2)
                        ? NULL : gfx_surface_check(L, 2);
    int width = (int)luaL_checkinteger(L, 3);

    /* Required with a surface and meaningless without one. Defaulted, it
     * would mean a call that drew nothing and said it had. */
    unsigned height = (unsigned)(s == NULL ? 0 : luaL_checkinteger(L, 4));

    if (d->page == NULL || d->page_width != width) {
        forget_layout(d);
        d->page = web_page_layout(L, d->dom, width);

        if (d->page == NULL) {
            return luaL_error(L, "no memory to lay the page out");
        }

        d->page_width = width;
    }

    /* And the band: the page's row at the surface's first, so a page longer
     * than any surface is painted a band at a time where it is read. */
    if (s != NULL) {
        web_page_paint(d->page, s, (long)luaL_optinteger(L, 5, 0), height);
    }

    lua_pushinteger(L, web_page_height(d->page));

    return 1;
}

/*
 * link_at(x, y) -> the href under that point, or nil.
 *
 * In *page* coordinates, which is what the caller has: it laid the page out
 * into a surface of its own and knows where in that surface the window is
 * looking. A browser converts a click once and this answers from the boxes.
 *
 * The href is whatever the document said, relative or absolute. Resolving
 * it against the page's own address is the caller's, because the caller is
 * the one that knows what that address was.
 */
static int l_link_at(lua_State *L)
{
    struct doc *d = checkdoc(L);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    size_t len = 0;
    const char *href;

    if (d->page == NULL) {
        lua_pushnil(L);
        return 1;
    }

    href = web_page_link_at(d->page, x, y, &len);

    if (href == NULL) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, href, len);
    }

    return 1;
}

/*
 * images() -> { { src = "...", x, y, w, h }, ... }, the pictures the layout
 * made boxes for, in page coordinates.
 *
 * The src is whatever the document said, as with `link_at`: fetching it is
 * the caller's, which knows the page's own address, and so is decoding it,
 * which `gfx` does. Empty before `render` has laid the page out.
 */
static int l_images(lua_State *L)
{
    struct doc *d = checkdoc(L);
    size_t i, n = web_page_images(d->page);

    lua_createtable(L, (int)n, 0);

    for (i = 0; i < n; i++) {
        int box[4];
        size_t len = 0;
        const char *src = web_page_image(d->page, i, &len, box);

        lua_createtable(L, 0, 5);
        lua_pushlstring(L, src, len);
        lua_setfield(L, -2, "src");
        lua_pushinteger(L, box[0]);
        lua_setfield(L, -2, "x");
        lua_pushinteger(L, box[1]);
        lua_setfield(L, -2, "y");
        lua_pushinteger(L, box[2]);
        lua_setfield(L, -2, "w");
        lua_pushinteger(L, box[3]);
        lua_setfield(L, -2, "h");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }

    return 1;
}

/*
 * `doc:ns_layout(width, height, address)` -> the page's height, and how wide
 * it reached - `width`, or more where a box overflows it - laid out by
 * NetSurf (`roadmap.md` 6zz j3); nil and why when it could not be. The
 * address is the one the page came from, which its links and its
 * stylesheets' `url()`s resolve against; the first call makes the box tree,
 * a later one at another width lays the same tree out again.
 */
static int l_ns_layout(lua_State *L)
{
    struct doc *d = checkdoc(L);
    int width = (int)luaL_checkinteger(L, 2);
    int height = (int)luaL_checkinteger(L, 3);
    const char *base = luaL_optstring(L, 4, NULL);
    int tall;

    if (d->ns == NULL) {
        d->ns = web_ns_open(d->dom, base, d->charset);

        if (d->ns == NULL) {
            lua_pushnil(L);
            lua_pushliteral(L, "NetSurf's layout is not set up: web.setup first");
            return 2;
        }
    }

    tall = web_ns_layout(d->ns, L, width, height);

    if (tall < 0) {
        lua_pushnil(L);
        lua_pushstring(L, web_ns_why(d->ns));
        return 2;
    }

    lua_pushinteger(L, tall);
    lua_pushinteger(L, web_ns_wide(d->ns));
    return 2;
}

/*
 * `doc:ns_sheets(address)` -> the page's linked stylesheets still to fetch,
 * `{ n = number, url = address }` each, in the order the cascade takes them;
 * and `doc:ns_sheet(n, text)` -> true, a fetched one made a sheet. Both
 * before the first `ns_layout`, which makes the cascade from them all.
 */
static int l_ns_sheets(lua_State *L)
{
    struct doc *d = checkdoc(L);
    const char *url = NULL;
    size_t k = 0, n;

    if (d->ns == NULL) {
        d->ns = web_ns_open(d->dom, luaL_optstring(L, 2, NULL), d->charset);
    }

    lua_newtable(L);

    while (d->ns != NULL && (n = web_ns_sheets(d->ns, k, &url)) != 0) {
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, (lua_Integer)n);
        lua_setfield(L, -2, "n");
        lua_pushstring(L, url);
        lua_setfield(L, -2, "url");
        lua_rawseti(L, -2, (lua_Integer)++k);
    }

    return 1;
}

static int l_ns_sheet(lua_State *L)
{
    struct doc *d = checkdoc(L);
    lua_Integer n = luaL_checkinteger(L, 2);
    size_t len = 0;
    const char *text = luaL_checklstring(L, 3, &len);

    lua_pushboolean(L, d->ns != NULL && n > 0
                       && web_ns_sheet(d->ns, (size_t)n, text, len));
    return 1;
}

/* `doc:ns_imports()` -> each sheet with an `@import` to fetch, `{ n, url }`;
 * `doc:ns_import(n, text)` hands it back - nil for one that could not be had
 * (`roadmap.md` 6zz j5). */
static int l_ns_imports(lua_State *L)
{
    struct doc *d = checkdoc(L);
    const char *url = NULL;
    size_t k = 0, n;

    lua_newtable(L);

    while (d->ns != NULL && (n = web_ns_imports(d->ns, k, &url)) != 0) {
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, (lua_Integer)n);
        lua_setfield(L, -2, "n");
        lua_pushstring(L, url);
        lua_setfield(L, -2, "url");
        lua_rawseti(L, -2, (lua_Integer)++k);
    }

    return 1;
}

static int l_ns_import(lua_State *L)
{
    struct doc *d = checkdoc(L);
    lua_Integer n = luaL_checkinteger(L, 2);
    size_t len = 0;
    const char *text = luaL_optlstring(L, 3, NULL, &len);

    lua_pushboolean(L, d->ns != NULL && n > 0
                       && web_ns_import(d->ns, (size_t)n, text, len));
    return 1;
}

/*
 * `doc:ns_objects()` -> the pictures the layout asked for, in page order:
 * `{ url, x, y, w, h, background, arrived }` each, where its box is now.
 * And `doc:ns_picture(k, surface, width, height)`: the `k`th arrived, its
 * natural size; the page wants laying out again after, since a picture the
 * page gave no size to takes its own.
 */
static int l_ns_objects(lua_State *L)
{
    struct doc *d = checkdoc(L);
    size_t k, n = d->ns != NULL ? web_ns_objects(d->ns) : 0;

    lua_createtable(L, (int)n, 0);

    for (k = 0; k < n; k++) {
        const char *url = NULL;
        int x = 0, y = 0, w = 0, h = 0;
        bool background = false, arrived = false;

        if (!web_ns_object(d->ns, k, &url, &x, &y, &w, &h, &background,
                           &arrived)) {
            break;
        }

        lua_createtable(L, 0, 7);
        lua_pushstring(L, url);
        lua_setfield(L, -2, "url");
        lua_pushinteger(L, x);
        lua_setfield(L, -2, "x");
        lua_pushinteger(L, y);
        lua_setfield(L, -2, "y");
        lua_pushinteger(L, w);
        lua_setfield(L, -2, "w");
        lua_pushinteger(L, h);
        lua_setfield(L, -2, "h");
        lua_pushboolean(L, background);
        lua_setfield(L, -2, "background");
        lua_pushboolean(L, arrived);
        lua_setfield(L, -2, "arrived");
        lua_rawseti(L, -2, (lua_Integer)k + 1);
    }

    return 1;
}

static int l_ns_picture(lua_State *L)
{
    struct doc *d = checkdoc(L);
    lua_Integer k = luaL_checkinteger(L, 2);
    int width = (int)luaL_checkinteger(L, 4);
    int height = (int)luaL_checkinteger(L, 5);

    (void)gfx_surface_check(L, 3);
    lua_pushvalue(L, 3);
    lua_pushboolean(L, d->ns != NULL && k >= 1
                       && web_ns_picture(d->ns, L, (size_t)(k - 1), width,
                                         height));
    return 1;
}

/* `doc:ns_paint(surface, width, height, from [, x, y, w, h])`: the band of
 * the page that starts `from` rows down, drawn into the surface by NetSurf -
 * all of it, or where it meets that area of the page. */
static int l_ns_paint(lua_State *L)
{
    struct doc *d = checkdoc(L);
    struct surface *s = gfx_surface_check(L, 2);
    int width = (int)luaL_checkinteger(L, 3);
    int height = (int)luaL_checkinteger(L, 4);
    long from = (long)luaL_optinteger(L, 5, 0);
    int area[4];
    bool some = !lua_isnoneornil(L, 6);

    if (some) {
        area[0] = (int)luaL_checkinteger(L, 6);
        area[1] = (int)luaL_checkinteger(L, 7);
        area[2] = (int)luaL_checkinteger(L, 8);
        area[3] = (int)luaL_checkinteger(L, 9);
    }

    if (d->ns != NULL) {
        web_ns_paint(d->ns, L, s, width, height, from, some ? area : NULL);
    }

    return 0;
}

/* `doc:ns_click(x, y)` -> what a press there did to a form field - "field",
 * "toggled", "sent" - or nil (`roadmap.md` 6zz j6). */
static int l_ns_click(lua_State *L)
{
    struct doc *d = checkdoc(L);
    const char *did = d->ns == NULL ? NULL
                      : web_ns_click(d->ns, L, (int)luaL_checkinteger(L, 2),
                                     (int)luaL_checkinteger(L, 3));

    if (did == NULL) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, did);
    }

    return 1;
}

/* `doc:ns_select()` -> the select pressed last: where it is on the page,
 * `multiple`, and `options`, each `{ text, chosen }` - or nil. */
static int l_ns_select(lua_State *L)
{
    struct doc *d = checkdoc(L);

    if (d->ns == NULL) {
        lua_pushnil(L);
        return 1;
    }

    return web_ns_select(d->ns, L);
}

/* `doc:ns_select_choose(i)` -> whether its `i`th option was chosen. */
static int l_ns_select_choose(lua_State *L)
{
    struct doc *d = checkdoc(L);

    lua_pushboolean(L, d->ns != NULL
                       && web_ns_select_choose(d->ns, L,
                                               (int)luaL_checkinteger(L, 2)));
    return 1;
}

/* `doc:ns_key(key)` -> whether the field with the caret took it; the key is
 * the kit's number for it (`keys.lua`). */
static int l_ns_key(lua_State *L)
{
    struct doc *d = checkdoc(L);

    lua_pushboolean(L, d->ns != NULL
                       && web_ns_key(d->ns, L,
                                     (int)luaL_checkinteger(L, 2)));
    return 1;
}

/* `doc:ns_focused()` -> whether a field has the caret. */
static int l_ns_focused(lua_State *L)
{
    struct doc *d = checkdoc(L);

    lua_pushboolean(L, d->ns != NULL && web_ns_focused(d->ns));
    return 1;
}

/* `doc:ns_blur()`: the caret out of its field. */
static int l_ns_blur(lua_State *L)
{
    struct doc *d = checkdoc(L);

    if (d->ns != NULL) {
        web_ns_blur(d->ns, L);
    }

    return 0;
}

/* `doc:ns_sent()` -> a form sent - its address, and for a POST its body and
 * its type - or nil; taken, so it is answered once. */
static int l_ns_sent(lua_State *L)
{
    struct doc *d = checkdoc(L);
    char *url = NULL, *body = NULL;
    const char *type = NULL;

    if (d->ns == NULL || !web_ns_sent(d->ns, &url, &body, &type)) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushstring(L, url);

    if (body != NULL) {
        lua_pushstring(L, body);
        lua_pushstring(L, type != NULL ? type : "");
    } else {
        lua_pushnil(L);
        lua_pushnil(L);
    }

    free(url);
    free(body);
    return 3;
}

/* One kind of cost into the table on top: `{ ticks, calls }`. */
static void push_cost(lua_State *L, const char *name,
                      const struct web_ns_cost *cost)
{
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, (lua_Integer)cost->ticks);
    lua_setfield(L, -2, "ticks");
    lua_pushinteger(L, (lua_Integer)cost->calls);
    lua_setfield(L, -2, "calls");
    lua_setfield(L, -2, name);
}

/* `doc:ns_costs()` -> what the last paint spent, by kind - `whole`, `fills`,
 * `text`, `pictures`, `scaled`, `shapes`, `other`, each `{ ticks, calls }` in counter
 * ticks (`roadmap.md` 6zz h). */
static int l_ns_costs(lua_State *L)
{
    struct doc *d = checkdoc(L);
    const struct web_ns_costs *c;

    if (d->ns == NULL) {
        lua_pushnil(L);
        return 1;
    }

    c = web_ns_costs(d->ns);
    lua_createtable(L, 0, 9);
    push_cost(L, "whole", &c->whole);
    push_cost(L, "fills", &c->fills);
    push_cost(L, "text", &c->text);
    push_cost(L, "pictures", &c->pictures);
    push_cost(L, "scaled", &c->scaled);
    push_cost(L, "shapes", &c->shapes);
    push_cost(L, "other", &c->other);
    push_cost(L, "boxes", &c->boxes);
    push_cost(L, "layout", &c->layout);
    return 1;
}

/* `doc:ns_dirty()` -> x, y, w, h of what changed on the page since last
 * asked, or nil. */
static int l_ns_dirty(lua_State *L)
{
    struct doc *d = checkdoc(L);
    int x, y, w, h;

    if (d->ns == NULL || !web_ns_dirty(d->ns, &x, &y, &w, &h)) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushinteger(L, x);
    lua_pushinteger(L, y);
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 4;
}

/* `doc:ns_link_at(x, y)` -> the address under a point of the page, whole,
 * or nil. */
static int l_ns_link_at(lua_State *L)
{
    struct doc *d = checkdoc(L);
    const char *href = d->ns == NULL ? NULL
                       : web_ns_link_at(d->ns, (int)luaL_checkinteger(L, 2),
                                        (int)luaL_checkinteger(L, 3));

    if (href == NULL) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, href);
    }

    return 1;
}

static int l_close(lua_State *L)
{
    struct doc *d = luaL_checkudata(L, 1, DOC_HANDLE);

    forget_layout(d);
    web_ns_close(d->ns, L);
    d->ns = NULL;

    if (d->dom != NULL) {
        dom_node_unref(d->dom);
        d->dom = NULL;
    }

    return 0;
}

/*
 * style(css, tag) -> the computed colour of the first such element.
 *
 * **The smallest thing that is evidence for the cascade.** Getting a colour
 * out means the selector matched a node in the tree, the cascade ran, and a
 * computed value came back - which is all thirty-six callbacks in
 * `web_select.c` doing their job, or enough of them to matter.
 */
static int l_style(lua_State *L)
{
    struct doc *d = checkdoc(L);
    size_t csslen = 0, taglen = 0;
    const char *css = luaL_checklstring(L, 2, &csslen);
    const char *tag = luaL_checklstring(L, 3, &taglen);
    css_select_handler *handler = web_select_handler();
    css_stylesheet *sheet;
    css_select_ctx *ctx = NULL;
    css_select_results *results = NULL;
    css_media media;
    css_unit_ctx units;
    dom_string *name;
    dom_nodelist *list = NULL;
    dom_node *node = NULL;
    uint32_t n = 0;
    css_color colour = 0;

    if (handler == NULL) {
        lua_pushnil(L);
        lua_pushliteral(L, "the selection handler could not be prepared");
        return 2;
    }

    sheet = web_style_sheet(css, csslen);

    if (sheet == NULL) {
        lua_pushnil(L);
        lua_pushliteral(L, "the stylesheet did not parse");
        return 2;
    }

    if (css_select_ctx_create(&ctx) != CSS_OK
        || css_select_ctx_append_sheet(ctx, sheet, CSS_ORIGIN_AUTHOR,
                                       NULL) != CSS_OK) {
        if (ctx != NULL) { css_select_ctx_destroy(ctx); }
        css_stylesheet_destroy(sheet);
        lua_pushnil(L);
        lua_pushliteral(L, "the stylesheet could not be applied");
        return 2;
    }

    name = to_dom(tag, taglen);

    if (name == NULL
        || dom_document_get_elements_by_tag_name(d->dom, name, &list)
           != DOM_NO_ERR || list == NULL) {
        if (name != NULL) { dom_string_unref(name); }
        css_select_ctx_destroy(ctx);
        css_stylesheet_destroy(sheet);
        lua_pushnil(L);
        lua_pushliteral(L, "the document could not be searched");
        return 2;
    }

    dom_string_unref(name);
    (void)dom_nodelist_get_length(list, &n);

    if (n == 0 || dom_nodelist_item(list, 0, &node) != DOM_NO_ERR
        || node == NULL) {
        dom_nodelist_unref(list);
        css_select_ctx_destroy(ctx);
        css_stylesheet_destroy(sheet);
        lua_pushnil(L);
        lua_pushliteral(L, "there is no such element");
        return 2;
    }

    /*
     * A screen, and a viewport to resolve `em` and percentages against.
     * Zeroing this would leave the default font size at nothing, and every
     * relative length would come back zero without saying why.
     */
    memset(&media, 0, sizeof(media));
    memset(&units, 0, sizeof(units));

    media.type = CSS_MEDIA_SCREEN;
    media.width = INTTOFIX(800);
    media.height = INTTOFIX(600);

    units.viewport_width     = INTTOFIX(800);
    units.viewport_height    = INTTOFIX(600);
    units.font_size_default  = INTTOFIX(16);
    units.font_size_minimum  = INTTOFIX(6);
    units.device_dpi         = INTTOFIX(96);

    if (css_select_style(ctx, node, &units, &media, NULL,
                         handler, NULL, &results) != CSS_OK
        || results == NULL
        || results->styles[CSS_PSEUDO_ELEMENT_NONE] == NULL) {
        if (results != NULL) { css_select_results_destroy(results); }
        dom_node_unref(node);
        dom_nodelist_unref(list);
        css_select_ctx_destroy(ctx);
        css_stylesheet_destroy(sheet);
        lua_pushnil(L);
        lua_pushliteral(L, "no style came back for it");
        return 2;
    }

    (void)css_computed_color(results->styles[CSS_PSEUDO_ELEMENT_NONE], &colour);

    css_select_results_destroy(results);
    dom_node_unref(node);
    dom_nodelist_unref(list);
    css_select_ctx_destroy(ctx);
    css_stylesheet_destroy(sheet);

    /*
     * `css_color` is 0xAARRGGBB; the alpha is dropped because nothing here
     * composites yet and a caller comparing strings should not have to.
     *
     * Written out by hand rather than with `lua_pushfstring`, which is
     * Lua's own miniature formatter and understands neither a width nor a
     * zero pad - `%02x` reaches it as an invalid option and raises. It is
     * not `snprintf` and the resemblance is the trap.
     */
    {
        static const char hex[] = "0123456789abcdef";
        char out[8];
        unsigned i;

        out[0] = '#';

        for (i = 0; i < 3; i++) {
            unsigned byte = (colour >> (16 - 8 * i)) & 0xffu;

            out[1 + i * 2] = hex[byte >> 4];
            out[2 + i * 2] = hex[byte & 0xf];
        }

        lua_pushlstring(L, out, 7);
    }

    return 1;
}

/*
 * events() -> made, skipped: the DOM mutation events libdom has made and
 * dispatched, and those it did not make because nothing could hear them
 * (`runtime/patches/netsurf/README.md`). A browser without JavaScript
 * should make none; the browser's suite holds it to that.
 */
static int l_events(lua_State *L)
{
    extern unsigned long _dom_events_made, _dom_events_skipped;

    lua_pushinteger(L, (lua_Integer)_dom_events_made);
    lua_pushinteger(L, (lua_Integer)_dom_events_skipped);
    return 2;
}

/* stylesheet(css) -> true, or nil and why. */
static int l_stylesheet(lua_State *L)
{
    size_t len = 0;
    const char *text = luaL_checklstring(L, 1, &len);
    css_stylesheet *sheet = web_style_sheet(text, len);

    if (sheet == NULL) {
        lua_pushnil(L);
        lua_pushliteral(L, "the stylesheet did not parse");
        return 2;
    }

    css_stylesheet_destroy(sheet);
    lua_pushboolean(L, 1);
    return 1;
}

void kosmos_web_kit(lua_State *L)
{
    static const luaL_Reg api[] = {
        { "parse",      l_parse },
        { "parser",     l_parser },
        { "stylesheet", l_stylesheet },
        { "events",     l_events },
        { "join",       web_netsurf_join },
        { "setup",      web_netsurf_setup },
        { "zoom",       web_netsurf_zoom },
        { "svg",        web_svg },
        { NULL, NULL }
    };

    static const luaL_Reg doc[] = {
        { "count", l_count },
        { "meta",  l_meta },
        { "lang",  l_lang },
        { "style", l_style },
        { "text",  l_text },
        { "render",  l_render },
        { "link_at", l_link_at },
        { "images", l_images },
        { "title", l_title },
        { "close", l_close },
        { "ns_layout", l_ns_layout },
        { "ns_paint", l_ns_paint },
        { "ns_link_at", l_ns_link_at },
        { "ns_sheets", l_ns_sheets },
        { "ns_sheet", l_ns_sheet },
        { "ns_imports", l_ns_imports },
        { "ns_import", l_ns_import },
        { "ns_objects", l_ns_objects },
        { "ns_picture", l_ns_picture },
        { "ns_click", l_ns_click },
        { "ns_select", l_ns_select },
        { "ns_select_choose", l_ns_select_choose },
        { "ns_key", l_ns_key },
        { "ns_focused", l_ns_focused },
        { "ns_blur", l_ns_blur },
        { "ns_sent", l_ns_sent },
        { "ns_dirty", l_ns_dirty },
        { "ns_costs", l_ns_costs },
        { NULL, NULL }
    };

    static const luaL_Reg parsing[] = {
        { "feed",   l_parsing_feed },
        { "finish", l_parsing_finish },
        { "refed",  l_parsing_refed },
        { NULL, NULL }
    };

    luaL_newmetatable(L, DOC_HANDLE);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_close);
    lua_setfield(L, -2, "__gc");
    luaL_setfuncs(L, doc, 0);
    lua_pop(L, 1);

    luaL_newmetatable(L, PARSING_HANDLE);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_parsing_gc);
    lua_setfield(L, -2, "__gc");
    luaL_setfuncs(L, parsing, 0);
    lua_pop(L, 1);

    web_svg_kit(L);
    luaL_newlib(L, api);
}
