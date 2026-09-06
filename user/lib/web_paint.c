/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A document, laid out into boxes and then painted from them.
 *
 * **The boxes are the whole point of this file.** It used to walk the tree
 * and draw as it went, which is the shortest way to get ink on a page and
 * throws away the one thing everything else needs: where each word ended
 * up. Nothing could be clicked, because nothing remembered which element a
 * word came from. Nothing could be bold inside a paragraph, because a block
 * was flattened to one string before it was measured. Nothing could keep
 * `pre`'s spaces, because there was nowhere to record that this block was
 * different. Six missing features, one missing data structure.
 *
 * So layout runs first and produces an array of *runs* - a positioned slice
 * of text with a face, an ink and, if it sits inside an `<a>`, a link. Paint
 * is then one loop over that array and knows nothing about the DOM. Hit
 * testing is the same loop with a comparison instead of a draw.
 *
 * It is still not CSS layout: there is no box model, no floats, no `width`,
 * and a face comes from the tag rather than from the cascade. What is right
 * is the shape - blocks stacked down the page, inline content flowing
 * inside them, lines broken at a measured width and aligned on a shared
 * baseline - so the engine that replaces this changes how a box is chosen
 * and not what a box is.
 *
 * **One crossing for the whole page.** Layout and painting both happen
 * inside C and what returns to Lua is a number. A call per box would cost
 * more than the drawing - `gfx.md` 19.11 puts a Lua/C crossing at about two
 * thousand pixels of work, and a page is hundreds of boxes and thousands of
 * glyphs. `docfont.c` took the same decision for a page of a PDF.
 *
 * The surface is the caller's and is scrolled by blitting out of it, which
 * is why a page is laid out *once* into something taller than the window
 * rather than re-laid on every scroll.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include <dom/dom.h>

#include "gfx_draw.h"
#include "web_paint.h"

/*
 * The faces a page needs, and there are seven.
 *
 * `gfx` holds twelve and the desktop's four roles are the first of them, so
 * eight are free. Headings step down in size and are bold; `strong` gets the
 * body's bold, `em` the italic, `code` and `pre` the monospace.
 *
 * Every inline face is at the body size, which is a real limitation and not
 * an oversight: `<em>` inside an `<h1>` comes out at sixteen pixels in the
 * middle of a twenty-eight pixel heading. Fixing it properly means a face
 * per (family, weight, slant, size) chosen by the cascade, which is the
 * work this file is a step towards rather than a substitute for.
 */
enum {
    FACE_H1, FACE_H2, FACE_H3, FACE_BODY, FACE_BOLD, FACE_ITALIC, FACE_MONO,
    FACE_COUNT
};

/* 0xAARRGGBB, the same order `css_color` uses and `gfx` expects. */
#define INK        0xff101010u
#define PAPER      0xffffffffu
#define QUOTE_INK  0xff404040u
#define RULE       0xffc0c0c0u
#define LINK_INK   0xff1a4fbfu

/* Bounded, because a browser is handed documents written to break it and
 * this process has a fixed stack with a guard page under it. */
#define MAX_DEPTH  64

#define LI_INDENT     24
#define QUOTE_INDENT  32

/*
 * One thing on the page.
 *
 * `len == 0` is a rectangle - a heading's rule, a list marker, a link's
 * underline - and anything else is a run of text. One array rather than
 * two, because painting is then a single loop in the order things were
 * laid out.
 */
struct run {
    int      x, y, w, h;
    int      face;
    uint32_t ink;
    size_t   at, len;           /* the text, into `page->text` */
    int      link;              /* into `page->links`, or -1 */
};

struct link {
    size_t at, len;             /* the href, into `page->text` */
};

struct web_page {
    int      face[FACE_COUNT];
    int      width;             /* the content width, in pixels */
    int      y;                 /* the pen, moving down the page */

    /*
     * Every word, copied once.
     *
     * A run could have held the `dom_string` its bytes came from, and then
     * the order two things are freed in would matter: unref the tree first
     * and every run points at nothing. Fifty kilobytes of text is fifty
     * kilobytes, and in exchange a page holds no reference to the document
     * it was made from.
     */
    char    *text;
    size_t   text_len, text_cap;

    struct run *runs;
    size_t   nruns, runs_cap;

    struct link *links;
    size_t   nlinks, links_cap;

    /* An allocation failed. The page is short rather than wrong, and the
     * caller is told by the height it gets back being what it is. */
    bool     full;
};

/*
 * Where the current line stands, while a block is being laid out.
 *
 * `first` is the index of the line's first run, which is what makes a
 * shared baseline possible: the tops cannot be assigned until the tallest
 * face on the line is known, and that is not known until the line ends.
 */
struct liner {
    int    indent;
    int    room;
    long   pen;                 /* x within the content box, from `indent` */
    size_t first;
    bool   any;                 /* anything on this line yet */
    bool   pending;             /* whitespace seen since the last word */
    int    space_face;          /* the face it was seen in */
};

/* What is in force for the text being walked: a face, an ink, and whether
 * this is inside a link. Passed down by value, so leaving an element
 * restores what was in force outside it without a stack of its own. */
struct inl {
    int      face;
    uint32_t ink;
    int      link;
};

/*--------------------------------------------------------------------------
 * Growable arrays.
 *------------------------------------------------------------------------*/

static bool grow(void **items, size_t *cap, size_t want, size_t size)
{
    size_t next = (*cap == 0) ? 64 : *cap;
    void *bigger;

    if (want <= *cap) {
        return true;
    }

    while (next < want) {
        next *= 2;
    }

    bigger = realloc(*items, next * size);

    if (bigger == NULL) {
        return false;
    }

    *items = bigger;
    *cap = next;

    return true;
}

/* Copies bytes into the page's text and returns where they landed, or
 * `(size_t)-1`. */
static size_t keep(struct web_page *p, const char *s, size_t len)
{
    if (!grow((void **)&p->text, &p->text_cap, p->text_len + len, 1)) {
        p->full = true;
        return (size_t)-1;
    }

    memcpy(p->text + p->text_len, s, len);
    p->text_len += len;

    return p->text_len - len;
}

static struct run *push_run(struct web_page *p)
{
    if (!grow((void **)&p->runs, &p->runs_cap, p->nruns + 1,
              sizeof(struct run))) {
        p->full = true;
        return NULL;
    }

    memset(&p->runs[p->nruns], 0, sizeof(struct run));
    p->runs[p->nruns].link = -1;

    return &p->runs[p->nruns++];
}

/* A rectangle: a rule, a bullet, an underline. */
static void push_rect(struct web_page *p, int x, int y, int w, int h,
                      uint32_t ink)
{
    struct run *r = push_run(p);

    if (r == NULL) {
        return;
    }

    r->x = x;
    r->y = y;
    r->w = w;
    r->h = h;
    r->ink = ink;
}

/*--------------------------------------------------------------------------
 * Tags.
 *------------------------------------------------------------------------*/

static bool is_element(dom_node *node)
{
    dom_node_type type;

    return node != NULL
        && dom_node_get_node_type(node, &type) == DOM_NO_ERR
        && type == DOM_ELEMENT_NODE;
}

/* The node's name, lowercased into `out`. libdom answers in uppercase for
 * HTML and every comparison here is against a lowercase literal. */
static size_t tag_of(dom_node *node, char *out, size_t cap)
{
    dom_string *name = NULL, *low = NULL;
    size_t n = 0;

    out[0] = '\0';

    if (dom_node_get_node_name(node, &name) != DOM_NO_ERR || name == NULL) {
        return 0;
    }

    if (dom_string_tolower(name, true, &low) == DOM_NO_ERR && low != NULL) {
        n = dom_string_byte_length(low);

        if (n >= cap) {
            n = cap - 1;
        }

        memcpy(out, dom_string_data(low), n);
        dom_string_unref(low);
    }

    dom_string_unref(name);
    out[n] = '\0';

    return n;
}

static bool is_tag(const char *tag, const char *want)
{
    return strcmp(tag, want) == 0;
}

static bool in_set(const char *tag, const char *const *set)
{
    unsigned i;

    for (i = 0; set[i] != NULL; i++) {
        if (strcmp(set[i], tag) == 0) {
            return true;
        }
    }

    return false;
}

/*
 * The tags that carry a paragraph's worth of text.
 *
 * A `div` is not one: it is a container, and treating it as a block would
 * lay out everything inside it and then lay each paragraph out again.
 */
static bool is_block(const char *tag)
{
    static const char *const tags[] = {
        "h1", "h2", "h3", "h4", "h5", "h6",
        "p", "li", "dt", "dd", "blockquote", "pre", "figcaption",
        NULL
    };

    return in_set(tag, tags);
}

/* Elements whose text is not content. Without this a `<script>` inside the
 * body is laid out as a paragraph of JavaScript. */
static bool is_hidden(const char *tag)
{
    static const char *const tags[] = {
        "script", "style", "head", "title", "noscript", "template", NULL
    };

    return in_set(tag, tags);
}

static int face_for(const struct web_page *p, const char *tag)
{
    if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && tag[2] == '\0') {
        if (tag[1] == '1') return p->face[FACE_H1];
        if (tag[1] == '2') return p->face[FACE_H2];
        return p->face[FACE_H3];        /* h3..h6 share a size */
    }

    if (is_tag(tag, "pre")) {
        return p->face[FACE_MONO];
    }

    if (is_tag(tag, "blockquote")) {
        return p->face[FACE_ITALIC];
    }

    return p->face[FACE_BODY];
}

/*--------------------------------------------------------------------------
 * Lines.
 *------------------------------------------------------------------------*/

/*
 * The line is finished: place its runs and move the pen down.
 *
 * **Tops are assigned here rather than when a run is made**, because two
 * faces on one line share a *baseline* and not a top edge - an `<em>` at
 * sixteen pixels next to `<code>` at fifteen sit on the same line, and
 * stacking them by their tops is the classic way to make a document look
 * subtly broken. The tallest ascent on the line is the baseline, and each
 * run's top is that ascent less its own.
 *
 * `gfx` adds the face's ascent inside its drawing routine, so a run's `y`
 * is a top and this is the only arithmetic that needs the ascent at all.
 */
static void end_line(struct web_page *p, struct liner *l)
{
    int ascent = 0, descent = 0;
    size_t i, end;

    if (!l->any) {
        return;
    }

    for (i = l->first; i < p->nruns; i++) {
        int a, d;

        if (p->runs[i].len == 0) {
            continue;
        }

        a = gfx_draw_ascent(p->runs[i].face);
        d = gfx_draw_height(p->runs[i].face) - a;

        if (a > ascent) ascent = a;
        if (d > descent) descent = d;
    }

    for (i = l->first; i < p->nruns; i++) {
        if (p->runs[i].len > 0) {
            p->runs[i].y = p->y + ascent - gfx_draw_ascent(p->runs[i].face);
        }
    }

    /*
     * Underlines, now that the baseline is known, and pushed rather than
     * drawn: painting happens later and from this array alone. `end` is
     * taken first so the loop does not underline its own underlines.
     *
     * One rule per *span* of the same link rather than one per word, or the
     * spaces between the words of a link come out unmarked and the link
     * reads as several short ones.
     */
    end = p->nruns;
    i = l->first;

    while (i < end) {
        int link = p->runs[i].link;
        size_t last = i;

        if (link < 0 || p->runs[i].len == 0) {
            i++;
            continue;
        }

        while (last + 1 < end && p->runs[last + 1].link == link
               && p->runs[last + 1].len > 0) {
            last++;
        }

        push_rect(p, p->runs[i].x, p->y + ascent + 1,
                  p->runs[last].x + p->runs[last].w - p->runs[i].x, 1,
                  p->runs[i].ink);

        i = last + 1;
    }

    p->y += ascent + descent;
    l->pen = 0;
    l->any = false;
    l->first = p->nruns;
}

/*
 * One word, at the pen, wrapping first if it does not fit.
 *
 * Greedy, one word at a time, each word measured once. Measuring the whole
 * candidate line on every word would be quadratic in a paragraph's length;
 * adding one advance at a time is what a line breaker does.
 */
static void emit_word(struct web_page *p, struct liner *l,
                      const struct inl *in, const char *word, size_t wlen)
{
    long w = gfx_draw_measure(in->face, word, wlen);
    long space = 0;
    struct run *r;
    size_t at;

    /*
     * The space is measured in the face it was *written* in, which is
     * neither the word before it nor the word after.
     *
     * `<code>h1</code> is set` puts the space in the paragraph's own text
     * node, so it is a paragraph-width space even though a monospace word
     * is on one side of it. Measuring it in the new word's face put a
     * monospace gap in front of every inline code span; measuring it in the
     * previous word's face moved the same gap to the other side. Only the
     * face at the point the whitespace was seen is right, and `put_words`
     * is where that is known.
     */
    if (l->any && l->pending) {
        space = gfx_draw_measure(l->space_face, " ", 1);
    }

    if (l->any && l->pen + space + w > l->room) {
        end_line(p, l);
        space = 0;
    }

    at = keep(p, word, wlen);

    if (at == (size_t)-1) {
        return;
    }

    r = push_run(p);

    if (r == NULL) {
        return;
    }

    r->x    = l->indent + (int)(l->pen + space);
    r->w    = (int)w;
    r->h    = gfx_draw_height(in->face);
    r->face = in->face;
    r->ink  = in->ink;
    r->at   = at;
    r->len  = wlen;
    r->link = in->link;

    l->pen += space + w;
    l->any = true;
    l->pending = false;
}

static bool is_space(char c)
{
    return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f';
}

/*
 * A text node's words, with runs of whitespace collapsed to one space.
 *
 * `pending` is what makes `one <em>two</em>` different from `one<em>two</em>`:
 * a space between two runs exists because the *source* had whitespace there,
 * and the source's whitespace is spread across three text nodes. Setting the
 * flag where the whitespace is, and consuming it where a word is emitted,
 * carries that across the element boundary without either side knowing about
 * the other.
 */
static void put_words(struct web_page *p, struct liner *l,
                      const struct inl *in, const char *text, size_t len)
{
    size_t at = 0;

    while (at < len) {
        size_t word, wlen;

        while (at < len && is_space(text[at])) {
            at++;
            l->pending = true;
            l->space_face = in->face;
        }

        word = at;

        while (at < len && !is_space(text[at])) {
            at++;
        }

        wlen = at - word;

        if (wlen == 0) {
            break;
        }

        emit_word(p, l, in, text + word, wlen);
    }
}

/*
 * A preformatted text node: one run per source line, spaces kept.
 *
 * No wrapping, because that is what `pre` means. A line wider than the page
 * runs off the right edge and is clipped by the surface, which is what a
 * browser without a horizontal scrollbar can honestly do.
 */
static void put_pre(struct web_page *p, struct liner *l,
                    const struct inl *in, const char *text, size_t len)
{
    size_t at = 0;

    /* HTML drops a newline immediately after the tag, and a document that
     * indents its markup ends the block with one too. */
    if (len > 0 && text[0] == '\n') {
        text++;
        len--;
    }

    while (len > 0 && text[len - 1] == '\n') {
        len--;
    }

    for (;;) {
        size_t start = at;

        while (at < len && text[at] != '\n') {
            at++;
        }

        if (at > start) {
            size_t off = keep(p, text + start, at - start);
            struct run *r;

            if (off == (size_t)-1) {
                return;
            }

            r = push_run(p);

            if (r == NULL) {
                return;
            }

            r->x    = l->indent;
            r->w    = (int)gfx_draw_measure(in->face, text + start,
                                            at - start);
            r->h    = gfx_draw_height(in->face);
            r->face = in->face;
            r->ink  = in->ink;
            r->at   = off;
            r->len  = at - start;
            r->link = in->link;

            l->any = true;
            end_line(p, l);
        } else {
            p->y += gfx_draw_height(in->face);      /* a blank line */
        }

        if (at >= len) {
            break;
        }

        at++;                                       /* past the newline */
    }
}

/*--------------------------------------------------------------------------
 * Inline content.
 *------------------------------------------------------------------------*/

/* The element's href, kept and registered, or -1 if it has none. */
static int href_of(struct web_page *p, dom_node *el)
{
    dom_string *name = NULL, *value = NULL;
    int out = -1;

    if (dom_string_create((const uint8_t *)"href", 4, &name) != DOM_NO_ERR) {
        return -1;
    }

    if (dom_element_get_attribute(el, name, &value) == DOM_NO_ERR
        && value != NULL && dom_string_byte_length(value) > 0) {
        size_t n = dom_string_byte_length(value);
        size_t at = keep(p, dom_string_data(value), n);

        if (at != (size_t)-1
            && grow((void **)&p->links, &p->links_cap, p->nlinks + 1,
                    sizeof(struct link))) {
            p->links[p->nlinks].at = at;
            p->links[p->nlinks].len = n;
            out = (int)p->nlinks++;
        }
    }

    if (value != NULL) {
        dom_string_unref(value);
    }

    dom_string_unref(name);

    return out;
}

/* What entering this element changes about the text inside it. */
static void enter(struct web_page *p, struct inl *in, dom_node *el,
                  const char *tag)
{
    static const char *const bold[]   = { "strong", "b", NULL };
    static const char *const italic[] = { "em", "i", "cite", "var", NULL };
    static const char *const mono[]   = { "code", "tt", "kbd", "samp", NULL };

    if (in_set(tag, bold)) {
        /*
         * A heading's face is already bold, and the body's bold inside one
         * would make the emphasised word *smaller* than the words around
         * it - which reads as a bug rather than as emphasis.
         */
        if (in->face != p->face[FACE_H1] && in->face != p->face[FACE_H2]
            && in->face != p->face[FACE_H3]) {
            in->face = p->face[FACE_BOLD];
        }
    } else if (in_set(tag, italic)) {
        in->face = p->face[FACE_ITALIC];
    } else if (in_set(tag, mono)) {
        in->face = p->face[FACE_MONO];
    } else if (is_tag(tag, "a")) {
        int at = href_of(p, el);

        if (at >= 0) {
            in->link = at;
            in->ink = LINK_INK;
        }
    }
}

static void inline_walk(struct web_page *p, struct liner *l, dom_node *node,
                        struct inl in, int depth, bool pre)
{
    dom_node *child = NULL;

    if (depth > MAX_DEPTH
        || dom_node_get_first_child(node, &child) != DOM_NO_ERR) {
        return;
    }

    while (child != NULL) {
        dom_node *next = NULL;
        dom_node_type type;

        if (dom_node_get_node_type(child, &type) == DOM_NO_ERR) {
            if (type == DOM_TEXT_NODE || type == DOM_CDATA_SECTION_NODE) {
                dom_string *str = NULL;

                if (dom_node_get_text_content(child, &str) == DOM_NO_ERR
                    && str != NULL) {
                    const char *data = dom_string_data(str);
                    size_t n = dom_string_byte_length(str);

                    if (pre) {
                        put_pre(p, l, &in, data, n);
                    } else {
                        put_words(p, l, &in, data, n);
                    }

                    dom_string_unref(str);
                }
            } else if (type == DOM_ELEMENT_NODE) {
                char tag[16];

                (void)tag_of(child, tag, sizeof(tag));

                if (!is_hidden(tag)) {
                    struct inl inner = in;

                    enter(p, &inner, child, tag);
                    inline_walk(p, l, child, inner, depth + 1, pre);
                }
            }
        }

        (void)dom_node_get_next_sibling(child, &next);
        dom_node_unref(child);
        child = next;
    }
}

/*--------------------------------------------------------------------------
 * Blocks.
 *------------------------------------------------------------------------*/

static void lay_out_block(struct web_page *p, dom_node *el, const char *tag)
{
    struct liner l;
    struct inl in;
    int face = face_for(p, tag);
    bool heading = (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6'
                    && tag[2] == '\0');
    bool bullet = is_tag(tag, "li");
    size_t began;
    size_t i;

    memset(&l, 0, sizeof(l));

    in.face = face;
    in.ink  = INK;
    in.link = -1;

    if (bullet) {
        l.indent = LI_INDENT;
    } else if (is_tag(tag, "blockquote")) {
        l.indent = QUOTE_INDENT;
        in.ink = QUOTE_INK;
    }

    /* Space above a heading, so it belongs to what follows it rather than
     * to what came before. */
    if (heading) {
        p->y += gfx_draw_height(face) / 2;
    }

    l.room  = p->width - l.indent;
    l.first = p->nruns;
    began   = p->nruns;

    inline_walk(p, &l, el, in, 0, is_tag(tag, "pre"));
    end_line(p, &l);

    /*
     * The bullet, after the fact, because where it goes depends on the
     * first line's baseline and that is not known until the line is closed.
     * There is no `list-style` here to consult: a marker is a box the
     * cascade chooses, and until the cascade is consulted a disc for every
     * item is the honest guess - it is wrong for `<ol>`, and says so.
     */
    if (bullet) {
        for (i = began; i < p->nruns; i++) {
            if (p->runs[i].len > 0) {
                push_rect(p, LI_INDENT - 14,
                          p->runs[i].y + p->runs[i].h / 2 - 2, 4, 4, INK);
                break;
            }
        }
    }

    /* And a rule under the biggest two, which is what a heading is for. */
    if (heading && (tag[1] == '1' || tag[1] == '2')) {
        push_rect(p, 0, p->y + 3, p->width, 1, RULE);
    }

    p->y += gfx_draw_height(p->face[FACE_BODY]) / 2;
}

/*
 * Lays out every block at or below `node`, and says whether it laid out any.
 *
 * The answer is what stops a `<blockquote>` wrapping a `<p>` from being
 * drawn twice: the recursion happens *first*, and an element is only laid
 * out as a block itself when nothing below it was. The old code flattened
 * every block to its text content, so a paragraph inside a quotation
 * appeared once as part of the quotation and again on its own.
 *
 * What it costs is a `<li>` that holds text *and* a nested list: the inner
 * items are laid out and the outer item's own words are not. Real layout
 * puts those words in an anonymous block, which is the machinery this file
 * does not have yet. Losing them is the smaller of the two wrongs, and it
 * is rarer.
 */
static bool layout_blocks(struct web_page *p, dom_node *node, int depth)
{
    dom_node *child = NULL;
    bool any = false;

    if (depth > MAX_DEPTH
        || dom_node_get_first_child(node, &child) != DOM_NO_ERR) {
        return false;
    }

    while (child != NULL) {
        dom_node *next = NULL;

        if (is_element(child)) {
            char tag[16];

            (void)tag_of(child, tag, sizeof(tag));

            if (!is_hidden(tag)) {
                bool below = layout_blocks(p, child, depth + 1);

                if (!below && is_block(tag)) {
                    lay_out_block(p, child, tag);
                    below = true;
                }

                any = any || below;
            }
        }

        (void)dom_node_get_next_sibling(child, &next);
        dom_node_unref(child);
        child = next;
    }

    return any;
}

/*--------------------------------------------------------------------------
 * The page.
 *------------------------------------------------------------------------*/

static void load_faces(lua_State *L, struct web_page *p)
{
    static const struct { const char *font; int px; } wanted[FACE_COUNT] = {
        { "ibmplexsans-bold",   28 },   /* h1 */
        { "ibmplexsans-bold",   22 },   /* h2 */
        { "ibmplexsans-bold",   18 },   /* h3 and below */
        { "ibmplexsans",        16 },   /* body */
        { "ibmplexsans-bold",   16 },   /* strong, b */
        { "ibmplexsans-italic", 16 },   /* em, i, blockquote */
        { "ibmplexmono",        15 },   /* pre, code */
    };
    unsigned i;

    for (i = 0; i < FACE_COUNT; i++) {
        lua_getglobal(L, "gfx");
        lua_getfield(L, -1, "face");
        lua_pushstring(L, wanted[i].font);
        lua_pushinteger(L, wanted[i].px);

        if (lua_pcall(L, 2, 1, 0) != LUA_OK || !lua_isinteger(L, -1)) {
            /* No such face, or the pool is full. The interface font is a
             * worse answer than the right one and a better one than none. */
            p->face[i] = 0;
            lua_pop(L, 2);
            continue;
        }

        p->face[i] = (int)lua_tointeger(L, -1);
        lua_pop(L, 2);
    }
}

struct web_page *web_page_layout(lua_State *L, void *document, int width)
{
    struct web_page *p = calloc(1, sizeof(*p));

    if (p == NULL) {
        return NULL;
    }

    p->width = width;
    p->y     = 8;

    load_faces(L, p);
    layout_blocks(p, (dom_node *)document, 0);

    return p;
}

void web_page_free(struct web_page *p)
{
    if (p == NULL) {
        return;
    }

    free(p->text);
    free(p->runs);
    free(p->links);
    free(p);
}

int web_page_height(const struct web_page *p)
{
    return (p == NULL) ? 0 : p->y;
}

void web_page_paint(const struct web_page *p, struct surface *s,
                    unsigned height)
{
    size_t i;

    if (p == NULL || s == NULL) {
        return;
    }

    gfx_draw_fill(s, 0, 0, p->width, (long)height, PAPER);

    for (i = 0; i < p->nruns; i++) {
        const struct run *r = &p->runs[i];

        /* Clipped here as well as inside `gfx`, because a page is laid out
         * taller than the surface it is painted into when the document is
         * taller than the ceiling the caller set. */
        if (r->y < 0 || (unsigned)r->y >= height) {
            continue;
        }

        if (r->len == 0) {
            gfx_draw_fill(s, r->x, r->y, r->w, r->h, r->ink);
        } else {
            gfx_draw_text(s, r->face, r->x, r->y, p->text + r->at, r->len,
                          r->ink, NULL);
        }
    }
}

const char *web_page_link_at(const struct web_page *p, int x, int y,
                             size_t *len)
{
    size_t i;

    if (p == NULL) {
        return NULL;
    }

    for (i = 0; i < p->nruns; i++) {
        const struct run *r = &p->runs[i];

        if (r->link < 0 || r->len == 0
            || x < r->x || x >= r->x + r->w
            || y < r->y || y >= r->y + r->h) {
            continue;
        }

        *len = p->links[r->link].len;

        return p->text + p->links[r->link].at;
    }

    return NULL;
}
