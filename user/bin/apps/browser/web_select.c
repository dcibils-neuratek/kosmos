/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The bridge between the tree and the cascade.
 *
 * libcss knows how to match a selector and how to cascade; it does not know
 * what a node is. So it asks - thirty-six questions, through this table -
 * and every answer here is a walk of a libdom tree. `select.h` is the whole
 * contract and this file is written against it rather than against anyone
 * else's implementation of it.
 *
 * **What is answered honestly and what is answered `false`.** Structure is
 * real: names, classes, ids, attributes, ancestors, siblings, position
 * among siblings, emptiness, roots. *State* is not, and cannot be yet -
 * hover, focus, active, visited, checked, target - because nothing in this
 * system has an interaction model for a document. They answer `false`,
 * which is the truth for a page nobody has pointed at, rather than an
 * approximation of one.
 *
 * `node_presentational_hint` returns none, so `bgcolor` and friends do
 * nothing. That is an HTML4 attribute path and it is a separate piece of
 * work from the cascade.
 *
 * **The case of a tag name is the trap.** libdom hands back an HTML
 * element's name in upper case, because that is what the DOM says
 * `nodeName` is; libcss lowercases selectors for an HTML document. Compared
 * raw, `p` never matches `P` and a stylesheet silently does nothing at all.
 * So names are lowered here, once, on the way out.
 */

#include <stddef.h>
#include <string.h>

#include <dom/dom.h>
#include <libcss/libcss.h>

#include "web_select.h"

/*
 * Interned once, because every one of them is asked for on every element of
 * every document and interning is a hash and an allocation.
 */
static lwc_string *str_class;
static lwc_string *str_id;
static lwc_string *str_href;
static dom_string *dom_class;
static dom_string *dom_id;
static dom_string *dom_href;
static dom_string *dom_nodedata;

static bool ready;

static bool intern_all(void)
{
    if (ready) {
        return true;
    }

    if (lwc_intern_string("class", 5, &str_class) != lwc_error_ok
        || lwc_intern_string("id", 2, &str_id) != lwc_error_ok
        || lwc_intern_string("href", 4, &str_href) != lwc_error_ok) {
        return false;
    }

    if (dom_string_create((const uint8_t *)"class", 5, &dom_class) != DOM_NO_ERR
        || dom_string_create((const uint8_t *)"id", 2, &dom_id) != DOM_NO_ERR
        || dom_string_create((const uint8_t *)"href", 4, &dom_href) != DOM_NO_ERR
        || dom_string_create((const uint8_t *)"_libcss", 7, &dom_nodedata)
           != DOM_NO_ERR) {
        return false;
    }

    ready = true;
    return true;
}

/* An element's name, lowered and interned. See the note about case above. */
static css_error name_of(dom_node *node, lwc_string **out)
{
    dom_string *name = NULL;
    dom_string *lower = NULL;
    lwc_error e;

    *out = NULL;

    if (dom_node_get_node_name(node, &name) != DOM_NO_ERR || name == NULL) {
        return CSS_NOMEM;
    }

    if (dom_string_tolower(name, true, &lower) != DOM_NO_ERR || lower == NULL) {
        dom_string_unref(name);
        return CSS_NOMEM;
    }

    dom_string_unref(name);

    e = lwc_intern_string(dom_string_data(lower),
                          dom_string_byte_length(lower), out);
    dom_string_unref(lower);

    return (e == lwc_error_ok) ? CSS_OK : CSS_NOMEM;
}

/* Is this node an element? Text and comments are asked about too. */
static bool is_element(dom_node *node)
{
    dom_node_type type;

    return node != NULL
        && dom_node_get_node_type(node, &type) == DOM_NO_ERR
        && type == DOM_ELEMENT_NODE;
}

static dom_string *attr_of(dom_node *node, dom_string *name)
{
    dom_string *value = NULL;

    if (!is_element(node)) {
        return NULL;
    }

    if (dom_element_get_attribute(node, name, &value) != DOM_NO_ERR) {
        return NULL;
    }

    return value;                       /* may be NULL, which means absent */
}

/* The previous *element* sibling: libcss's "sibling" never means text. */
static dom_node *previous_element(dom_node *node)
{
    dom_node *at = NULL;

    if (dom_node_get_previous_sibling(node, &at) != DOM_NO_ERR) {
        return NULL;
    }

    while (at != NULL && !is_element(at)) {
        dom_node *before = NULL;

        (void)dom_node_get_previous_sibling(at, &before);
        dom_node_unref(at);
        at = before;
    }

    return at;                          /* referenced, or NULL */
}

/*--------------------------------------------------------------------------
 * The thirty-six.
 *------------------------------------------------------------------------*/

static css_error h_node_name(void *pw, void *node, css_qname *qname)
{
    (void)pw;

    qname->ns = NULL;
    return name_of(node, &qname->name);
}

static css_error h_node_classes(void *pw, void *node,
                                lwc_string ***classes, uint32_t *n_classes)
{
    dom_string *value;
    const char *at;
    size_t len;
    uint32_t count = 0;
    lwc_string **out = NULL;
    size_t i, start;

    (void)pw;

    *classes = NULL;
    *n_classes = 0;

    value = attr_of(node, dom_class);

    if (value == NULL) {
        return CSS_OK;
    }

    at = dom_string_data(value);
    len = dom_string_byte_length(value);

    /* Counted first, then filled: the list is handed to libcss whole and
     * growing it a word at a time would be an allocation per class. */
    for (i = 0, start = 0; i <= len; i++) {
        bool blank = (i == len) || at[i] == ' ' || at[i] == '\t'
                     || at[i] == '\n' || at[i] == '\r';

        if (blank) {
            if (i > start) { count++; }
            start = i + 1;
        }
    }

    if (count == 0) {
        dom_string_unref(value);
        return CSS_OK;
    }

    out = malloc(count * sizeof(*out));

    if (out == NULL) {
        dom_string_unref(value);
        return CSS_NOMEM;
    }

    count = 0;

    for (i = 0, start = 0; i <= len; i++) {
        bool blank = (i == len) || at[i] == ' ' || at[i] == '\t'
                     || at[i] == '\n' || at[i] == '\r';

        if (blank) {
            if (i > start
                && lwc_intern_string(at + start, i - start,
                                     &out[count]) == lwc_error_ok) {
                count++;
            }

            start = i + 1;
        }
    }

    dom_string_unref(value);

    *classes = out;
    *n_classes = count;

    return CSS_OK;
}

static css_error h_node_id(void *pw, void *node, lwc_string **id)
{
    dom_string *value = attr_of(node, dom_id);

    (void)pw;
    *id = NULL;

    if (value == NULL) {
        return CSS_OK;
    }

    (void)lwc_intern_string(dom_string_data(value),
                            dom_string_byte_length(value), id);
    dom_string_unref(value);

    return CSS_OK;
}

/* Does this element's name match, without allocating a comparison? */
static bool named(dom_node *node, const css_qname *qname)
{
    lwc_string *name = NULL;
    bool same = false;

    if (!is_element(node) || name_of(node, &name) != CSS_OK) {
        return false;
    }

    (void)lwc_string_isequal(name, qname->name, &same);
    lwc_string_unref(name);

    return same;
}

static css_error h_named_ancestor_node(void *pw, void *node,
                                       const css_qname *qname, void **ancestor)
{
    dom_node *at = node;

    (void)pw;
    *ancestor = NULL;

    dom_node_ref(at);

    for (;;) {
        dom_node *up = NULL;

        if (dom_node_get_parent_node(at, &up) != DOM_NO_ERR || up == NULL) {
            dom_node_unref(at);
            return CSS_OK;
        }

        dom_node_unref(at);
        at = up;

        if (!is_element(at)) {
            dom_node_unref(at);
            return CSS_OK;
        }

        if (named(at, qname)) {
            *ancestor = at;
            dom_node_unref(at);         /* the caller does not own it */
            return CSS_OK;
        }
    }
}

static css_error h_named_parent_node(void *pw, void *node,
                                     const css_qname *qname, void **parent)
{
    dom_node *up = NULL;

    (void)pw;
    *parent = NULL;

    if (dom_node_get_parent_node(node, &up) != DOM_NO_ERR || up == NULL) {
        return CSS_OK;
    }

    if (is_element(up) && named(up, qname)) {
        *parent = up;
    }

    dom_node_unref(up);
    return CSS_OK;
}

static css_error h_named_sibling_node(void *pw, void *node,
                                      const css_qname *qname, void **sibling)
{
    dom_node *before = previous_element(node);

    (void)pw;
    *sibling = NULL;

    if (before == NULL) {
        return CSS_OK;
    }

    if (named(before, qname)) {
        *sibling = before;
    }

    dom_node_unref(before);
    return CSS_OK;
}

static css_error h_named_generic_sibling_node(void *pw, void *node,
                                              const css_qname *qname,
                                              void **sibling)
{
    dom_node *at = previous_element(node);

    (void)pw;
    *sibling = NULL;

    while (at != NULL) {
        dom_node *before;

        if (named(at, qname)) {
            *sibling = at;
            dom_node_unref(at);
            return CSS_OK;
        }

        before = previous_element(at);
        dom_node_unref(at);
        at = before;
    }

    return CSS_OK;
}

static css_error h_parent_node(void *pw, void *node, void **parent)
{
    dom_node *up = NULL;

    (void)pw;
    *parent = NULL;

    if (dom_node_get_parent_node(node, &up) == DOM_NO_ERR && up != NULL) {
        if (is_element(up)) {
            *parent = up;
        }

        dom_node_unref(up);
    }

    return CSS_OK;
}

static css_error h_sibling_node(void *pw, void *node, void **sibling)
{
    dom_node *before = previous_element(node);

    (void)pw;
    *sibling = before;

    if (before != NULL) {
        dom_node_unref(before);
    }

    return CSS_OK;
}

static css_error h_node_has_name(void *pw, void *node,
                                 const css_qname *qname, bool *match)
{
    lwc_string *name = NULL;

    (void)pw;
    *match = false;

    /* The universal selector arrives as `*` and matches anything. */
    if (lwc_string_length(qname->name) == 1
        && *lwc_string_data(qname->name) == '*') {
        *match = true;
        return CSS_OK;
    }

    if (name_of(node, &name) != CSS_OK) {
        return CSS_OK;
    }

    (void)lwc_string_isequal(name, qname->name, match);
    lwc_string_unref(name);

    return CSS_OK;
}

static css_error h_node_has_class(void *pw, void *node,
                                  lwc_string *name, bool *match)
{
    lwc_string **classes = NULL;
    uint32_t n = 0, i;

    *match = false;

    if (h_node_classes(pw, node, &classes, &n) != CSS_OK) {
        return CSS_OK;
    }

    for (i = 0; i < n; i++) {
        bool same = false;

        (void)lwc_string_isequal(classes[i], name, &same);

        if (same) {
            *match = true;
        }

        lwc_string_unref(classes[i]);
    }

    free(classes);
    return CSS_OK;
}

static css_error h_node_has_id(void *pw, void *node,
                               lwc_string *name, bool *match)
{
    lwc_string *id = NULL;

    *match = false;
    (void)h_node_id(pw, node, &id);

    if (id != NULL) {
        (void)lwc_string_isequal(id, name, match);
        lwc_string_unref(id);
    }

    return CSS_OK;
}

/* One lookup for every attribute test, with the comparison left to the
 * caller: `[a=b]`, `[a^=b]` and the rest differ only in that. */
static dom_string *attr_named(dom_node *node, const css_qname *qname)
{
    dom_string *name = NULL;
    dom_string *value;

    if (dom_string_create((const uint8_t *)lwc_string_data(qname->name),
                          lwc_string_length(qname->name), &name) != DOM_NO_ERR) {
        return NULL;
    }

    value = attr_of(node, name);
    dom_string_unref(name);

    return value;
}

static css_error h_node_has_attribute(void *pw, void *node,
                                      const css_qname *qname, bool *match)
{
    dom_string *value = attr_named(node, qname);

    (void)pw;
    *match = (value != NULL);

    if (value != NULL) {
        dom_string_unref(value);
    }

    return CSS_OK;
}

static css_error h_node_has_attribute_equal(void *pw, void *node,
                                            const css_qname *qname,
                                            lwc_string *want, bool *match)
{
    dom_string *value = attr_named(node, qname);

    (void)pw;
    *match = false;

    if (value == NULL) {
        return CSS_OK;
    }

    *match = dom_string_byte_length(value) == lwc_string_length(want)
             && memcmp(dom_string_data(value), lwc_string_data(want),
                       lwc_string_length(want)) == 0;

    dom_string_unref(value);
    return CSS_OK;
}

/*
 * The four substring tests, and the one that is not.
 *
 * `dashmatch` is `[lang|=en]`: equal, or equal up to a hyphen. `includes`
 * is `[class~=x]`: one of a space-separated list. The other three are
 * prefix, suffix and substring, which are what they sound like.
 */
static css_error h_node_has_attribute_dashmatch(void *pw, void *node,
                                                const css_qname *qname,
                                                lwc_string *want, bool *match)
{
    dom_string *value = attr_named(node, qname);
    size_t n;

    (void)pw;
    *match = false;

    if (value == NULL) {
        return CSS_OK;
    }

    n = lwc_string_length(want);

    if (dom_string_byte_length(value) >= n
        && memcmp(dom_string_data(value), lwc_string_data(want), n) == 0
        && (dom_string_byte_length(value) == n
            || dom_string_data(value)[n] == '-')) {
        *match = true;
    }

    dom_string_unref(value);
    return CSS_OK;
}

static css_error h_node_has_attribute_includes(void *pw, void *node,
                                               const css_qname *qname,
                                               lwc_string *want, bool *match)
{
    dom_string *value = attr_named(node, qname);
    const char *at, *w;
    size_t len, n, i, start;

    (void)pw;
    *match = false;

    if (value == NULL) {
        return CSS_OK;
    }

    at = dom_string_data(value);
    len = dom_string_byte_length(value);
    w = lwc_string_data(want);
    n = lwc_string_length(want);

    for (i = 0, start = 0; i <= len && n > 0; i++) {
        bool blank = (i == len) || at[i] == ' ' || at[i] == '\t'
                     || at[i] == '\n' || at[i] == '\r';

        if (blank) {
            if (i - start == n && memcmp(at + start, w, n) == 0) {
                *match = true;
            }

            start = i + 1;
        }
    }

    dom_string_unref(value);
    return CSS_OK;
}

static css_error h_node_has_attribute_prefix(void *pw, void *node,
                                             const css_qname *qname,
                                             lwc_string *want, bool *match)
{
    dom_string *value = attr_named(node, qname);
    size_t n;

    (void)pw;
    *match = false;

    if (value == NULL) {
        return CSS_OK;
    }

    n = lwc_string_length(want);

    *match = n > 0 && dom_string_byte_length(value) >= n
             && memcmp(dom_string_data(value), lwc_string_data(want), n) == 0;

    dom_string_unref(value);
    return CSS_OK;
}

static css_error h_node_has_attribute_suffix(void *pw, void *node,
                                             const css_qname *qname,
                                             lwc_string *want, bool *match)
{
    dom_string *value = attr_named(node, qname);
    size_t n, len;

    (void)pw;
    *match = false;

    if (value == NULL) {
        return CSS_OK;
    }

    n = lwc_string_length(want);
    len = dom_string_byte_length(value);

    *match = n > 0 && len >= n
             && memcmp(dom_string_data(value) + len - n,
                       lwc_string_data(want), n) == 0;

    dom_string_unref(value);
    return CSS_OK;
}

static css_error h_node_has_attribute_substring(void *pw, void *node,
                                                const css_qname *qname,
                                                lwc_string *want, bool *match)
{
    dom_string *value = attr_named(node, qname);
    size_t n, len, i;

    (void)pw;
    *match = false;

    if (value == NULL) {
        return CSS_OK;
    }

    n = lwc_string_length(want);
    len = dom_string_byte_length(value);

    for (i = 0; n > 0 && i + n <= len; i++) {
        if (memcmp(dom_string_data(value) + i, lwc_string_data(want), n) == 0) {
            *match = true;
            break;
        }
    }

    dom_string_unref(value);
    return CSS_OK;
}

static css_error h_node_is_root(void *pw, void *node, bool *match)
{
    dom_node *up = NULL;

    (void)pw;
    *match = false;

    if (dom_node_get_parent_node(node, &up) == DOM_NO_ERR && up != NULL) {
        *match = !is_element(up);       /* the document is not an element */
        dom_node_unref(up);
    } else {
        *match = true;
    }

    return CSS_OK;
}

/*
 * How many element siblings, for `:nth-child` and its family.
 *
 * `after` counts forwards instead of back, and `same_name` restricts the
 * count to elements with this one's name - which is the difference between
 * `nth-child` and `nth-of-type`.
 */
static css_error h_node_count_siblings(void *pw, void *node, bool same_name,
                                       bool after, int32_t *count)
{
    lwc_string *want = NULL;
    dom_node *at = NULL;
    int32_t n = 0;

    (void)pw;
    *count = 0;

    if (same_name && name_of(node, &want) != CSS_OK) {
        return CSS_OK;
    }

    if (after) {
        if (dom_node_get_next_sibling(node, &at) != DOM_NO_ERR) {
            at = NULL;
        }
    } else {
        at = previous_element(node);
    }

    while (at != NULL) {
        dom_node *step = NULL;

        if (is_element(at)) {
            bool same = true;

            if (same_name) {
                lwc_string *name = NULL;

                same = false;

                if (name_of(at, &name) == CSS_OK) {
                    (void)lwc_string_isequal(name, want, &same);
                    lwc_string_unref(name);
                }
            }

            if (same) {
                n++;
            }
        }

        if (after) {
            (void)dom_node_get_next_sibling(at, &step);
        } else {
            step = previous_element(at);
        }

        dom_node_unref(at);
        at = step;
    }

    if (want != NULL) {
        lwc_string_unref(want);
    }

    *count = n;
    return CSS_OK;
}

static css_error h_node_is_empty(void *pw, void *node, bool *match)
{
    dom_node *child = NULL;

    (void)pw;
    *match = true;

    if (dom_node_get_first_child(node, &child) != DOM_NO_ERR) {
        return CSS_OK;
    }

    while (child != NULL) {
        dom_node *next = NULL;
        dom_node_type type;

        if (dom_node_get_node_type(child, &type) == DOM_NO_ERR
            && (type == DOM_ELEMENT_NODE || type == DOM_TEXT_NODE)) {
            *match = false;
            dom_node_unref(child);
            return CSS_OK;
        }

        (void)dom_node_get_next_sibling(child, &next);
        dom_node_unref(child);
        child = next;
    }

    return CSS_OK;
}

static css_error h_node_is_link(void *pw, void *node, bool *match)
{
    lwc_string *name = NULL;
    dom_string *href;

    (void)pw;
    *match = false;

    if (name_of(node, &name) != CSS_OK) {
        return CSS_OK;
    }

    if (lwc_string_length(name) == 1 && *lwc_string_data(name) == 'a') {
        href = attr_of(node, dom_href);

        if (href != NULL) {
            *match = true;
            dom_string_unref(href);
        }
    }

    lwc_string_unref(name);
    return CSS_OK;
}

/*
 * State, and there is none.
 *
 * A document nobody has pointed at, tabbed to, or followed a link out of.
 * `false` is the truth for that rather than an approximation of it, and
 * when there is an interaction model these are where it arrives.
 */
static css_error h_false(void *pw, void *node, bool *match)
{
    (void)pw; (void)node;
    *match = false;
    return CSS_OK;
}

static css_error h_node_is_lang(void *pw, void *node,
                                lwc_string *lang, bool *match)
{
    (void)pw; (void)node; (void)lang;
    *match = false;
    return CSS_OK;
}

/* No HTML4 presentational attributes: `bgcolor` and its friends are a
 * separate path from the cascade and are not walked yet. */
static css_error h_node_presentational_hint(void *pw, void *node,
                                            uint32_t *nhints, css_hint **hints)
{
    (void)pw; (void)node;
    *nhints = 0;
    *hints = NULL;
    return CSS_OK;
}

/*
 * The three defaults libcss asks for, and it asks for exactly three -
 * `colour`, `font-family` and `quotes`. Two of the three call sites treat
 * an error as fatal, so all three answer.
 */
static css_error h_ua_default_for_property(void *pw, uint32_t property,
                                           css_hint *hint)
{
    (void)pw;

    if (property == CSS_PROP_COLOR) {
        hint->data.color = 0xff000000;          /* opaque black */
        hint->status = CSS_COLOR_COLOR;
        return CSS_OK;
    }

    if (property == CSS_PROP_FONT_FAMILY) {
        hint->data.strings = NULL;
        hint->status = CSS_FONT_FAMILY_SANS_SERIF;
        return CSS_OK;
    }

    if (property == CSS_PROP_QUOTES) {
        hint->data.strings = NULL;
        hint->status = CSS_QUOTES_NONE;
        return CSS_OK;
    }

    return CSS_INVALID;
}

/*
 * libcss's own cache, hung off the node.
 *
 * It stores what it worked out about a node so a second pass does not
 * repeat it. libdom already has somewhere to put that - user data, keyed by
 * a string - so this is two calls rather than a table of our own.
 */
static css_error h_set_libcss_node_data(void *pw, void *node, void *data)
{
    void *old = NULL;

    (void)pw;
    (void)dom_node_set_user_data(node, dom_nodedata, data, NULL, &old);

    return CSS_OK;
}

static css_error h_get_libcss_node_data(void *pw, void *node, void **data)
{
    (void)pw;
    *data = NULL;
    (void)dom_node_get_user_data(node, dom_nodedata, data);

    return CSS_OK;
}

static css_select_handler handler = {
    CSS_SELECT_HANDLER_VERSION_1,
    h_node_name,
    h_node_classes,
    h_node_id,
    h_named_ancestor_node,
    h_named_parent_node,
    h_named_sibling_node,
    h_named_generic_sibling_node,
    h_parent_node,
    h_sibling_node,
    h_node_has_name,
    h_node_has_class,
    h_node_has_id,
    h_node_has_attribute,
    h_node_has_attribute_equal,
    h_node_has_attribute_dashmatch,
    h_node_has_attribute_includes,
    h_node_has_attribute_prefix,
    h_node_has_attribute_suffix,
    h_node_has_attribute_substring,
    h_node_is_root,
    h_node_count_siblings,
    h_node_is_empty,
    h_node_is_link,
    h_false,                    /* visited */
    h_false,                    /* hover */
    h_false,                    /* active */
    h_false,                    /* focus */
    h_false,                    /* enabled */
    h_false,                    /* disabled */
    h_false,                    /* checked */
    h_false,                    /* target */
    h_node_is_lang,
    h_node_presentational_hint,
    h_ua_default_for_property,
    h_set_libcss_node_data,
    h_get_libcss_node_data,
};

css_select_handler *web_select_handler(void)
{
    return intern_all() ? &handler : NULL;
}
