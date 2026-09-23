/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The web kit: HTML into a document, and CSS into a stylesheet.
 *
 * `use("/kits/web")`, the same way `/kits/pdf` and `/kits/gl` are reached,
 * and for the same reason: which language a library is written in is not a
 * fact its caller should have to know.
 *
 * **This is the smallest thing that is evidence.** Five libraries compiling
 * and linking says nothing about whether they run - a freestanding libc can
 * satisfy every symbol and still hand back a null on the first allocation.
 * So this parses a document and reports what is in it, and parses a
 * stylesheet and reports whether it was understood. Everything else the
 * browser needs is built on top of those two answers.
 *
 * What is deliberately not here yet: *selection*. Matching a selector
 * against a document means a `css_select_handler` - about thirty callbacks
 * bridging libdom's tree to libcss's questions - and that is the real
 * integration between the two, worth its own step rather than being smuggled
 * into the one that proves the parsers work.
 */

#include <stddef.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include <dom/dom.h>
#include <dom/bindings/hubbub/parser.h>
#include <libcss/libcss.h>
#include <libcss/fpmath.h>
#include <libcss/unit.h>

#include "web_select.h"
#include "web_paint.h"

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
 * parse(html) -> document, or nil and why.
 *
 * One chunk, because a Lua string is already whole. The streaming shape the
 * parser offers is what a fetch wants and this is not one.
 */
static int l_parse(lua_State *L)
{
    size_t len = 0;
    const char *html = luaL_checklstring(L, 1, &len);
    dom_hubbub_parser_params params;
    dom_hubbub_parser *parser = NULL;
    dom_document *document = NULL;
    struct doc *d;

    memset(&params, 0, sizeof(params));

    params.enc           = NULL;     /* detect it, which is what a browser does */
    params.fix_enc       = true;
    params.enable_script = false;    /* there is no interpreter to enable */
    params.msg           = NULL;
    params.ctx           = NULL;
    params.daf           = NULL;

    if (dom_hubbub_parser_create(&params, &parser, &document) != DOM_HUBBUB_OK) {
        lua_pushnil(L);
        lua_pushliteral(L, "the parser could not be created");
        return 2;
    }

    if (dom_hubbub_parser_parse_chunk(parser, (const uint8_t *)html, len)
        != DOM_HUBBUB_OK) {
        dom_hubbub_parser_destroy(parser);
        lua_pushnil(L);
        lua_pushliteral(L, "the document could not be parsed");
        return 2;
    }

    (void)dom_hubbub_parser_completed(parser);
    dom_hubbub_parser_destroy(parser);

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
    luaL_setmetatable(L, DOC_HANDLE);

    return 1;
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

/* text(tag) -> the text inside the first such element. */
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
 * The tags that carry a paragraph's worth of text.
 *
 * Deliberately not "every block-level element": a `div` holding three `p`s
 * would emit the whole page and then each paragraph again, so what is
 * listed here is the leaves - the elements a reader sees as a block rather
 * than the ones that group them.
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

/*
 * Depth-first, in document order, and *bounded*.
 *
 * A browser is handed documents written to break it, and this process has a
 * fixed stack with a guard page under it: a thousand nested divs would be a
 * fault rather than an error. Sixty-four is deeper than any document a
 * person writes and shallower than anything that could hurt.
 */
static void collect(lua_State *L, dom_node *node, int depth, int *n)
{
    dom_node *child = NULL;

    if (depth > 64) {
        return;
    }

    if (dom_node_get_first_child(node, &child) != DOM_NO_ERR) {
        return;
    }

    while (child != NULL) {
        dom_node *next = NULL;
        dom_node_type type;

        if (dom_node_get_node_type(child, &type) == DOM_NO_ERR
            && type == DOM_ELEMENT_NODE) {
            dom_string *name = NULL;

            if (dom_node_get_node_name(child, &name) == DOM_NO_ERR
                && name != NULL) {
                dom_string *lower = NULL;

                if (dom_string_tolower(name, true, &lower) == DOM_NO_ERR
                    && lower != NULL) {
                    if (is_block(dom_string_data(lower),
                                 dom_string_byte_length(lower))) {
                        dom_string *text = NULL;

                        if (dom_node_get_text_content(child, &text) == DOM_NO_ERR
                            && text != NULL) {
                            lua_createtable(L, 0, 2);

                            lua_pushlstring(L, dom_string_data(lower),
                                            dom_string_byte_length(lower));
                            lua_setfield(L, -2, "tag");

                            lua_pushlstring(L, dom_string_data(text),
                                            dom_string_byte_length(text));
                            lua_setfield(L, -2, "text");

                            lua_rawseti(L, -2, ++(*n));
                            dom_string_unref(text);
                        }
                    }

                    dom_string_unref(lower);
                }

                dom_string_unref(name);
            }
        }

        collect(L, child, depth + 1, n);

        (void)dom_node_get_next_sibling(child, &next);
        dom_node_unref(child);
        child = next;
    }
}

/*
 * blocks() -> { {tag = "h1", text = "..."}, ... } in document order.
 *
 * The first thing above "here is all the text" and below a layout engine:
 * structure without geometry. An application can space a heading differently
 * from a paragraph with it, which is most of what makes a page readable,
 * and none of it needs a box tree.
 */
static int l_blocks(lua_State *L)
{
    struct doc *d = checkdoc(L);
    int n = 0;

    lua_newtable(L);
    collect(L, (dom_node *)d->dom, 0, &n);

    return 1;
}

/*
 * render(surface, width [, height]) -> the height it used.
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
                        ? NULL : luaL_checkudata(L, 2, "kosmos.surface");
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

    if (s != NULL) {
        web_page_paint(d->page, s, height);
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

static int l_close(lua_State *L)
{
    struct doc *d = luaL_checkudata(L, 1, DOC_HANDLE);

    forget_layout(d);

    if (d->dom != NULL) {
        dom_node_unref(d->dom);
        d->dom = NULL;
    }

    return 0;
}

/*
 * A stylesheet needs somewhere to resolve a relative URL to, and libcss will
 * not create one without the callback even for a sheet that imports nothing.
 *
 * This hands the relative reference straight back. That is honest for what
 * this does today - parse a sheet and say whether it was understood - and
 * it is exactly the piece that has to become real when `@import` does.
 */
static css_error resolve_url(void *pw, const char *base,
                             lwc_string *rel, lwc_string **abs)
{
    (void)pw;
    (void)base;

    *abs = lwc_string_ref(rel);
    return CSS_OK;
}

/* One sheet, parsed and finished, or NULL. Shared by `stylesheet` and the
 * selection below, which needs exactly the same thing. */
static css_stylesheet *sheet_from(const char *text, size_t len)
{
    css_stylesheet_params params;
    css_stylesheet *sheet = NULL;
    css_error error;

    memset(&params, 0, sizeof(params));

    params.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    params.level          = CSS_LEVEL_DEFAULT;
    params.charset        = NULL;
    params.url            = "";
    params.title          = NULL;
    params.resolve        = resolve_url;

    if (css_stylesheet_create(&params, &sheet) != CSS_OK) {
        return NULL;
    }

    error = css_stylesheet_append_data(sheet, (const uint8_t *)text, len);

    /* CSS_NEEDDATA is what "keep going" looks like and is not a failure:
     * the parser says so after every chunk that did not end the sheet. */
    if (error != CSS_OK && error != CSS_NEEDDATA) {
        css_stylesheet_destroy(sheet);
        return NULL;
    }

    if (css_stylesheet_data_done(sheet) != CSS_OK) {
        css_stylesheet_destroy(sheet);
        return NULL;
    }

    return sheet;
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

    sheet = sheet_from(css, csslen);

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

/* stylesheet(css) -> true, or nil and why. */
static int l_stylesheet(lua_State *L)
{
    size_t len = 0;
    const char *text = luaL_checklstring(L, 1, &len);
    css_stylesheet *sheet = sheet_from(text, len);

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
        { "stylesheet", l_stylesheet },
        { NULL, NULL }
    };

    static const luaL_Reg doc[] = {
        { "count", l_count },
        { "style", l_style },
        { "text",   l_text },
        { "blocks", l_blocks },
        { "render",  l_render },
        { "link_at", l_link_at },
        { "title", l_title },
        { "close", l_close },
        { NULL, NULL }
    };

    luaL_newmetatable(L, DOC_HANDLE);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_close);
    lua_setfield(L, -2, "__gc");
    luaL_setfuncs(L, doc, 0);
    lua_pop(L, 1);

    luaL_newlib(L, api);
}
