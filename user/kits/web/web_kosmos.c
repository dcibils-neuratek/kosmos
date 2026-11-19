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
 * What is built on top of them now is the page laid out and drawn by
 * NetSurf (`web_netsurf.c`), and its SVGs (`web_svg.c`).
 *
 * **Mail is its one user now** - the browser it was built for was removed
 * in 0.11.461 - and what only that browser called went in the review
 * before 0.12.0 (`docs/cleanup.md`): the incremental parser, a stylesheet
 * alone, the DOM's event counts, `meta`, `lang`, and the forms, sheets,
 * imports, keys and costs of a NetSurf document - and, by Diego's decision
 * 2, Kosmos's own HTML layout (`web_paint.c`, `web_style.c`,
 * `web_select.c`), its probe `make web`, and the questions only that probe
 * asked: `count`, `text`, `title`, `style`, `render`, `link_at`, `images`.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include <dom/dom.h>
#include <dom/bindings/hubbub/parser.h>

#include "web_netsurf.h"
#include "web_svg.h"
#include "kits/gfx/gfx_draw.h"

#define DOC_HANDLE  "kosmos.dom"

struct doc {
    dom_document *dom;      /* NULL once closed */

    /* The document laid out by NetSurf (`roadmap.md` 6zz j3), made the
     * first time `ns_layout` is asked. */
    struct web_ns_doc *ns;

    /* The charset it was read in, which its forms are sent in too
     * (`roadmap.md` 6zz j7). */
    char charset[64];
};

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
 * **A parse, begun, fed and finished.** `web.parse` feeds it a whole
 * message once. It was built to be fed a page as it arrived, for the
 * browser (`roadmap.md` 6zz l1); the pieces are kept because hubbub's
 * encoding change below needs them either way.
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

    /* Zeroed before anything reads it: `__gc` is `close`, and close frees
     * NetSurf's layout. An uninitialised pointer there is a free of a
     * random word. */
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

    web_ns_close(d->ns, L);
    d->ns = NULL;

    if (d->dom != NULL) {
        dom_node_unref(d->dom);
        d->dom = NULL;
    }

    return 0;
}

void kosmos_web_kit(lua_State *L)
{
    static const luaL_Reg api[] = {
        { "parse",      l_parse },
        { "setup",      web_netsurf_setup },
        { "svg",        web_svg },
        { NULL, NULL }
    };

    static const luaL_Reg doc[] = {
        { "close", l_close },
        { "ns_layout", l_ns_layout },
        { "ns_paint", l_ns_paint },
        { "ns_link_at", l_ns_link_at },
        { "ns_objects", l_ns_objects },
        { "ns_picture", l_ns_picture },
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
    lua_pop(L, 1);

    web_svg_kit(L);
    luaL_newlib(L, api);
}
