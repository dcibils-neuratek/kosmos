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

#define DOC_HANDLE  "kosmos.dom"

struct doc {
    dom_document *dom;      /* NULL once closed */
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

    d = lua_newuserdatauv(L, sizeof(*d), 0);
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

static int l_close(lua_State *L)
{
    struct doc *d = luaL_checkudata(L, 1, DOC_HANDLE);

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

/* stylesheet(css) -> true, or nil and why. */
static int l_stylesheet(lua_State *L)
{
    size_t len = 0;
    const char *text = luaL_checklstring(L, 1, &len);
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
        lua_pushnil(L);
        lua_pushliteral(L, "the stylesheet could not be created");
        return 2;
    }

    error = css_stylesheet_append_data(sheet, (const uint8_t *)text, len);

    /* CSS_NEEDDATA is what "keep going" looks like, and is not a failure:
     * the parser says so after every chunk that did not end the sheet. */
    if (error != CSS_OK && error != CSS_NEEDDATA) {
        css_stylesheet_destroy(sheet);
        lua_pushnil(L);
        lua_pushliteral(L, "the stylesheet could not be read");
        return 2;
    }

    error = css_stylesheet_data_done(sheet);
    css_stylesheet_destroy(sheet);

    if (error != CSS_OK) {
        lua_pushnil(L);
        lua_pushliteral(L, "the stylesheet did not parse");
        return 2;
    }

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
        { "text",  l_text },
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
