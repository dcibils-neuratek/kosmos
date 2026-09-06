/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The cascade, actually consulted.
 *
 * libcss has parsed, selected and answered since it was vendored, and
 * nothing asked it: `web_paint.c` chose a face by tag name and an ink from
 * a `#define`, so a document's own stylesheet was read, understood and
 * discarded. This is the piece that closes that.
 *
 * **The defaults are a stylesheet now, not a switch.** What used to be
 * `face_for()` - h1 is 28 pixels and bold, `pre` is monospace, a quotation
 * is italic - is the user-agent sheet below, at UA origin. That is how a
 * browser has always expressed it, and it buys the thing a switch could
 * not: an author who writes `h1 { font-size: 40px }` wins, because the
 * cascade already knows UA loses to author. No code here knows that rule;
 * libcss does.
 *
 * One select context per document, built once. `l_style` builds one per
 * call and destroys it, which is right for a probe answering a single
 * question and would be absurd per element on a page of hundreds.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <dom/dom.h>
#include <libcss/libcss.h>
#include <libcss/fpmath.h>

#include "web_select.h"
#include "web_style.h"

/*
 * The user-agent stylesheet.
 *
 * Every number in here was a constant in `web_paint.c` an hour ago. Moving
 * them costs nothing and means a page can override any of them, which is
 * what a stylesheet in a document is for.
 *
 * Deliberately small: this is the shape of what a browser ships, not the
 * whole of one. `display: none` on the elements whose text is not content
 * replaces a hardcoded list of tag names with the reason they were on it.
 */
static const char UA_SHEET[] =
    "html, body { color: #101010; font-family: sans-serif; font-size: 16px }"
    "h1 { font-size: 28px; font-weight: bold }"
    "h2 { font-size: 22px; font-weight: bold }"
    "h3, h4, h5, h6 { font-size: 18px; font-weight: bold }"
    "b, strong { font-weight: bold }"
    "i, em, cite, var { font-style: italic }"
    "code, kbd, samp, tt, pre { font-family: monospace; font-size: 15px }"
    "blockquote { font-style: italic; color: #404040 }"
    "a { color: #1a4fbf }"
    "script, style, head, title, noscript, template { display: none }";

/* How many sheets a document may bring, past the user-agent one. A page
 * with more `<style>` blocks than this is styled by the first eight, which
 * is a limit worth having over an allocation that grows without one. */
#define SHEETS_MAX  8

struct web_style {
    css_select_ctx *ctx;
    css_stylesheet *sheets[SHEETS_MAX + 1];      /* +1 for the UA sheet */
    unsigned        nsheets;

    css_media       media;
    css_unit_ctx    units;
};

/*
 * A relative URL resolved against nothing, because nothing here loads a
 * second file. libcss will not create a sheet without the callback even for
 * one that imports nothing, and this is exactly the piece that becomes real
 * when `@import` does.
 */
static css_error resolve_url(void *pw, const char *base,
                             lwc_string *rel, lwc_string **abs)
{
    (void)pw;
    (void)base;

    *abs = lwc_string_ref(rel);

    return CSS_OK;
}

static css_stylesheet *sheet_from(const char *text, size_t len, bool ua)
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

    (void)ua;

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

static bool add_sheet(struct web_style *s, const char *text, size_t len,
                      css_origin origin)
{
    css_stylesheet *sheet;

    if (s->nsheets > SHEETS_MAX) {
        return false;
    }

    sheet = sheet_from(text, len, origin == CSS_ORIGIN_UA);

    if (sheet == NULL) {
        return false;
    }

    if (css_select_ctx_append_sheet(s->ctx, sheet, origin, NULL) != CSS_OK) {
        css_stylesheet_destroy(sheet);
        return false;
    }

    s->sheets[s->nsheets++] = sheet;

    return true;
}

/* Every `<style>` in the document, in order, at author origin. */
static void add_document_sheets(struct web_style *s, dom_document *doc)
{
    dom_string *name = NULL;
    dom_nodelist *list = NULL;
    uint32_t n = 0, i;

    if (dom_string_create((const uint8_t *)"style", 5, &name) != DOM_NO_ERR) {
        return;
    }

    if (dom_document_get_elements_by_tag_name(doc, name, &list) != DOM_NO_ERR
        || list == NULL) {
        dom_string_unref(name);
        return;
    }

    dom_string_unref(name);
    (void)dom_nodelist_get_length(list, &n);

    for (i = 0; i < n; i++) {
        dom_node *node = NULL;
        dom_string *text = NULL;

        if (dom_nodelist_item(list, i, &node) != DOM_NO_ERR || node == NULL) {
            continue;
        }

        if (dom_node_get_text_content(node, &text) == DOM_NO_ERR
            && text != NULL) {
            (void)add_sheet(s, dom_string_data(text),
                            dom_string_byte_length(text), CSS_ORIGIN_AUTHOR);
            dom_string_unref(text);
        }

        dom_node_unref(node);
    }

    dom_nodelist_unref(list);
}

struct web_style *web_style_open(void *document)
{
    struct web_style *s;

    if (web_select_handler() == NULL) {
        return NULL;
    }

    s = calloc(1, sizeof(*s));

    if (s == NULL) {
        return NULL;
    }

    if (css_select_ctx_create(&s->ctx) != CSS_OK) {
        free(s);
        return NULL;
    }

    /*
     * A screen, and a viewport to resolve `em` and percentages against.
     * Zeroing this would leave the default font size at nothing and every
     * relative length would come back zero without saying why.
     */
    s->media.type = CSS_MEDIA_SCREEN;
    s->media.width = INTTOFIX(1024);
    s->media.height = INTTOFIX(768);

    s->units.viewport_width    = INTTOFIX(1024);
    s->units.viewport_height   = INTTOFIX(768);
    s->units.font_size_default = INTTOFIX(16);
    s->units.font_size_minimum = INTTOFIX(6);
    s->units.device_dpi        = INTTOFIX(96);

    if (!add_sheet(s, UA_SHEET, sizeof(UA_SHEET) - 1, CSS_ORIGIN_UA)) {
        web_style_close(s);
        return NULL;
    }

    add_document_sheets(s, (dom_document *)document);

    return s;
}

void web_style_close(struct web_style *s)
{
    unsigned i;

    if (s == NULL) {
        return;
    }

    if (s->ctx != NULL) {
        css_select_ctx_destroy(s->ctx);
    }

    for (i = 0; i < s->nsheets; i++) {
        css_stylesheet_destroy(s->sheets[i]);
    }

    free(s);
}

/* A computed length in pixels. `css_fixed` is fixed point and the unit is
 * whatever the author wrote, so this is the one place either is seen. */
static int pixels_of(const struct web_style *s, css_fixed length,
                     css_unit unit, int fallback)
{
    /*
     * `(style, ctx, length, unit)` - and this had the last two the wrong
     * way round, which is a swap the compiler cannot see: `css_fixed` and
     * `css_unit` are both integers.
     *
     * It converted a length of `CSS_UNIT_PX`, which is zero, using a unit
     * of 45056, which is nothing - and answered zero. Every size then fell
     * through the range check below to the inherited one, so colour and
     * weight arrived from the cascade and *only* size did not. That is what
     * the probe said: kind 10, meaning the property was set, and 0 pixels.
     */
    css_fixed px = css_unit_len2device_px(NULL, &s->units, length, unit);
    int out = FIXTOINT(px);

    if (out < 1 || out > 400) {
        return fallback;
    }

    return out;
}

bool web_style_of(struct web_style *s, void *element, struct web_look *out)
{
    css_select_results *results = NULL;
    const css_computed_style *style;
    css_color colour = 0;
    css_fixed length = 0;
    css_unit unit = CSS_UNIT_PX;
    uint8_t got;

    if (s == NULL || element == NULL) {
        return false;
    }

    if (css_select_style(s->ctx, (dom_node *)element, &s->units, &s->media,
                         NULL, web_select_handler(), NULL, &results) != CSS_OK
        || results == NULL
        || results->styles[CSS_PSEUDO_ELEMENT_NONE] == NULL) {
        if (results != NULL) {
            css_select_results_destroy(results);
        }

        return false;
    }

    style = results->styles[CSS_PSEUDO_ELEMENT_NONE];

    if (css_computed_color(style, &colour) == CSS_COLOR_COLOR) {
        out->colour = (uint32_t)colour;
    }

    got = css_computed_font_size(style, &length, &unit);

    if (got == CSS_FONT_SIZE_DIMENSION) {
        out->px = pixels_of(s, length, unit, out->px);
    }

    /*
     * **Only what this element actually specifies**, and that is what makes
     * inheritance work without composing styles by hand.
     *
     * libcss answers `INHERIT` for a property no rule set, and the caller
     * passes in the look it inherited - so leaving `out` alone is exactly
     * "keep the parent's". Overwriting unconditionally would have made
     * every element that says nothing about weight *not bold*, which turns
     * inheritance into its opposite: a `<strong>` containing an `<a>` would
     * have unbolded the link.
     */
    got = css_computed_font_weight(style);

    if (got != CSS_FONT_WEIGHT_INHERIT) {
        /* Bold at 600 and above, which is where a family with more than
         * two weights puts the boundary - and these have two. */
        out->bold = (got == CSS_FONT_WEIGHT_BOLD
                     || got == CSS_FONT_WEIGHT_BOLDER
                     || got == CSS_FONT_WEIGHT_600
                     || got == CSS_FONT_WEIGHT_700
                     || got == CSS_FONT_WEIGHT_800
                     || got == CSS_FONT_WEIGHT_900);
    }

    got = css_computed_font_style(style);

    if (got != CSS_FONT_STYLE_INHERIT) {
        out->italic = (got == CSS_FONT_STYLE_ITALIC
                       || got == CSS_FONT_STYLE_OBLIQUE);
    }

    /*
     * `names` is written to unconditionally, so it cannot be NULL - the
     * generated getter stores the list before it returns the keyword, and
     * passing nothing for it is a write to address zero. Which is what it
     * did: `far 0x0` at `get_font_family`, on the first page rendered.
     *
     * The list itself is not used. There is no font matching by name here:
     * six files in the image, and what decides between them is whether the
     * computed family is monospace.
     */
    {
        lwc_string **names = NULL;

        got = css_computed_font_family(style, &names);

        if (got != CSS_FONT_FAMILY_INHERIT) {
            out->mono = (got == CSS_FONT_FAMILY_MONOSPACE);
        }
    }

    out->hidden = (css_computed_display(style, false) == CSS_DISPLAY_NONE);

    css_select_results_destroy(results);

    return true;
}
