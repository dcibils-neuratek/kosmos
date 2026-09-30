/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What NetSurf's layout stands on, when the browser around it is Kosmos's.
 *
 * `runtime/upstream/netsurf/netsurf/` is NetSurf 3.11's layout engine as it
 * was released (`roadmap.md` 6zz j): the box tree, layout, tables, flex and
 * the drawing of boxes, with the CSS selection and the few utilities they
 * use. It was written inside a browser, and it calls that browser: to fetch
 * a picture, to ask how wide a content is, to make a scrollbar, to measure
 * a word. This file is that browser, as much of it as the layout reaches.
 *
 * Three kinds of answer, and each says which it is:
 *
 *   - **The platform's**, the two tables every NetSurf front end provides:
 *     `misc.schedule`, which runs work later - here, when the caller drains
 *     the queue - and `layout`, which measures text, on `gfx`.
 *   - **Kosmos's own**, small functions NetSurf keeps in files that are
 *     mostly something else: `utils.c`, which is POSIX stand-ins this
 *     system does not have and does not want (`stat`, `scandir`, `uname`),
 *     and `idna.c`, which needs a Unicode library that is not here.
 *   - **Stand-ins**, for what a browser without scripts or form editing
 *     does not do yet: scrollbars inside a page, text areas, the text
 *     selection, visited links. Each does nothing and says so, so that
 *     what the layout does with its answer is what it does with a browser
 *     that has none of those - which is a case its authors handle.
 *
 * **j1** (`roadmap.md` 6zz j1): enough for it all to link. Nothing calls it
 * yet; j2 gives the text real faces and j3 drives a document through it.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "lauxlib.h"

#include <dom/dom.h>
#include <libcss/libcss.h>
#include <libcss/fpmath.h>

#include "utils/corestrings.h"
#include "utils/errors.h"
#include "utils/idna.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/nsurl.h"
#include "utils/string.h"
#include "utils/nsoption.h"
#include "utils/talloc.h"
#include "netsurf/browser_window.h"
#include "netsurf/content.h"
#include "netsurf/content_type.h"
#include "netsurf/layout.h"
#include "netsurf/misc.h"
#include "netsurf/plot_style.h"
#include "netsurf/plotters.h"
#include "netsurf/url_db.h"
#include "content/content_factory.h"
#include "content/textsearch.h"
#include "desktop/gui_internal.h"
#include "desktop/gui_table.h"
#include "desktop/print.h"
#include "desktop/scrollbar.h"
#include "desktop/selection.h"
#include "desktop/textarea.h"
#include "html/private.h"
#include "html/box_textarea.h"
#include "html/form_internal.h"
#include "html/html.h"
#include "html/interaction.h"
#include "html/object.h"
#include "html/box.h"
#include "html/box_construct.h"
#include "html/box_inspect.h"
#include "html/layout.h"
#include "css/hints.h"
#include "css/internal.h"
#include "css/utils.h"

#include "kits/gfx/gfx_draw.h"
#include "web_netsurf.h"

/*--------------------------------------------------------------------------
 * Once, before anything of NetSurf's runs: its interned names, which its
 * URLs, its styles and its boxes all compare against, and the table its
 * selection fills with an old document's presentational hints - `align`,
 * `bgcolor`, `width` - which NetSurf's `css.c` makes at start-up.
 *------------------------------------------------------------------------*/

static bool ready;

/* Two attribute names the form stand-ins below ask about, which are not
 * among NetSurf's own. */
static dom_string *dom_checked, *dom_multiple;

static bool netsurf_ready(void)
{
    if (!ready && corestrings_init() == NSERROR_OK
        && css_hint_init() == NSERROR_OK
        && dom_string_create((const uint8_t *)"checked", 7, &dom_checked)
           == DOM_NO_ERR
        && dom_string_create((const uint8_t *)"multiple", 8, &dom_multiple)
           == DOM_NO_ERR) {
        ready = true;
    }

    return ready;
}

/*
 * `web.join(base, href)`, the first of NetSurf's code the browser uses
 * (`roadmap.md` 6zz j1): a link resolved against its page as NetSurf
 * resolves one - `..` and `.` taken out, a scheme-relative `//host` given
 * the page's scheme, a fragment kept. `browser.lua`'s `resolve` does it in
 * Lua; this is the one NetSurf's layout will hand the pictures it asks for.
 */
int web_netsurf_join(lua_State *L)
{
    const char *base = luaL_checkstring(L, 1);
    const char *href = luaL_checkstring(L, 2);
    nsurl *page = NULL, *joined = NULL;
    nserror err;

    if (!netsurf_ready()) {
        return luaL_error(L, "NetSurf's names could not be made");
    }

    if (nsurl_create(base, &page) != NSERROR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not an address", base);
        return 2;
    }

    err = nsurl_join(page, href, &joined);
    nsurl_unref(page);

    if (err != NSERROR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s cannot be joined to %s", href, base);
        return 2;
    }

    lua_pushstring(L, nsurl_access(joined));
    nsurl_unref(joined);
    return 1;
}

/*--------------------------------------------------------------------------
 * The platform's: running work later.
 *
 * NetSurf builds a box tree a slice at a time, scheduling the next slice so
 * that a browser with a window to keep alive can draw between them. Here a
 * document is laid out in one go, so `schedule` queues and the caller
 * drains the queue to the end (`web_netsurf_run`). A negative time takes a
 * callback off the queue, as NetSurf's own scheduler does.
 *------------------------------------------------------------------------*/

struct later {
    void (*callback)(void *p);
    void *p;
};

static struct later *queue;
static size_t queued, room;

static nserror schedule(int t, void (*callback)(void *p), void *p)
{
    size_t i;

    if (t < 0) {
        for (i = 0; i < queued; i++) {
            if (queue[i].callback == callback && queue[i].p == p) {
                memmove(&queue[i], &queue[i + 1],
                        (queued - i - 1) * sizeof(*queue));
                queued--;
                i--;
            }
        }

        return NSERROR_OK;
    }

    if (queued == room) {
        size_t more = room ? room * 2 : 16;
        struct later *grown = realloc(queue, more * sizeof(*queue));

        if (grown == NULL) {
            return NSERROR_NOMEM;
        }

        queue = grown;
        room = more;
    }

    queue[queued].callback = callback;
    queue[queued].p = p;
    queued++;

    return NSERROR_OK;
}

void web_netsurf_run(void)
{
    while (queued > 0) {
        struct later next = queue[0];

        memmove(&queue[0], &queue[1], (queued - 1) * sizeof(*queue));
        queued--;
        next.callback(next.p);
    }
}

/*--------------------------------------------------------------------------
 * The platform's: measuring text, in the face it will be drawn in
 * (`roadmap.md` 6zz j2).
 *
 * A face is `gfx`'s - a font file rasterised at a size - and asking for one
 * is a call into Lua (`gfx.face`), which is not something to do for every
 * word the layout measures. So a face is asked for once per kind and size
 * and kept: six kinds, the files the image carries - sans in four weights
 * and slants, mono in two; there is no serif in the image, and a serif page
 * is drawn in sans rather than in boxes - and sizes rounded to the ladder
 * `web_paint.c` rounds to, which is what keeps the set bounded.
 *
 * `faces_L` is the Lua state the kit was called from, set for the length of
 * a layout or a painting and nowhere else.
 *------------------------------------------------------------------------*/

static lua_State *faces_L;

static const int LADDER[] = {
    9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 48, 64
};

#define RUNGS  (sizeof(LADDER) / sizeof(LADDER[0]))
#define KINDS  6

static const char *const KIND_FILE[KINDS] = {
    "ibmplexsans", "ibmplexsans-bold", "ibmplexsans-italic",
    "ibmplexsans-bolditalic", "ibmplexmono", "ibmplexmono-bold",
};

static int faces[KINDS][RUNGS];
static bool faces_known[KINDS][RUNGS];

/* A style's size in points, as pixels at the reference 96 to the inch,
 * and the rung of the ladder at or above it. */
static unsigned rung_of(const plot_font_style_t *fstyle)
{
    int px = (int)((fstyle->size * 96 / 72 + PLOT_STYLE_SCALE / 2)
                   / PLOT_STYLE_SCALE);
    unsigned i;

    for (i = 0; i < RUNGS; i++) {
        if (px <= LADDER[i]) {
            return i;
        }
    }

    return RUNGS - 1;
}

static unsigned kind_of(const plot_font_style_t *fstyle)
{
    bool bold = fstyle->weight >= 600;
    bool slanted = (fstyle->flags & (FONTF_ITALIC | FONTF_OBLIQUE)) != 0;

    if (fstyle->family == PLOT_FONT_FAMILY_MONOSPACE) {
        return bold ? 5 : 4;
    }

    return (bold ? 1u : 0u) + (slanted ? 2u : 0u);
}

static int face_for(const plot_font_style_t *fstyle)
{
    unsigned kind = kind_of(fstyle), rung = rung_of(fstyle);
    lua_State *L = faces_L;

    if (faces_known[kind][rung] || L == NULL) {
        return faces[kind][rung];
    }

    lua_getglobal(L, "gfx");
    lua_getfield(L, -1, "face");
    lua_pushstring(L, KIND_FILE[kind]);
    lua_pushinteger(L, LADDER[rung]);

    if (lua_pcall(L, 2, 1, 0) == LUA_OK && lua_isinteger(L, -1)) {
        faces[kind][rung] = (int)lua_tointeger(L, -1);
        faces_known[kind][rung] = true;
    }

    lua_pop(L, 2);
    return faces[kind][rung];
}

/* The length of the UTF-8 character that starts at `s`, at least one. */
static size_t char_length(const char *s, size_t left)
{
    unsigned char c = (unsigned char)s[0];
    size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;

    return n <= left ? n : left;
}

static nserror measure_width(const plot_font_style_t *fstyle,
                             const char *string, size_t length, int *width)
{
    *width = (int)gfx_draw_measure(face_for(fstyle), string, length);
    return NSERROR_OK;
}

/* The character whose middle is nearest `x`, and where it starts. */
static nserror measure_position(const plot_font_style_t *fstyle,
                                const char *string, size_t length, int x,
                                size_t *char_offset, int *actual_x)
{
    int face = face_for(fstyle);
    size_t at = 0;
    int pen = 0;

    while (at < length) {
        size_t n = char_length(string + at, length - at);
        int w = (int)gfx_draw_measure(face, string + at, n);

        if (pen + w / 2 > x) {
            break;
        }

        pen += w;
        at += n;
    }

    *char_offset = at;
    *actual_x = pen;
    return NSERROR_OK;
}

/*
 * Where a line should break to fit `x`: at the last space before the text
 * passes it, or - when no space comes before - at the end, which the
 * layout takes as "does not split". The same rule NetSurf's framebuffer
 * front end keeps.
 */
static nserror measure_split(const plot_font_style_t *fstyle,
                             const char *string, size_t length, int x,
                             size_t *char_offset, int *actual_x)
{
    int face = face_for(fstyle);
    size_t at = 0, space_at = 0;
    int pen = 0, space_x = 0;

    while (at < length) {
        size_t n = char_length(string + at, length - at);

        if (string[at] == ' ') {
            space_x = pen;
            space_at = at;
        }

        pen += (int)gfx_draw_measure(face, string + at, n);

        if (pen > x && space_at != 0) {
            *char_offset = space_at;
            *actual_x = space_x;
            return NSERROR_OK;
        }

        at += n;
    }

    *char_offset = length;
    *actual_x = pen;
    return NSERROR_OK;
}

static struct gui_misc_table misc_table = {
    .schedule = schedule,
};

static struct gui_layout_table layout_table = {
    .width = measure_width,
    .position = measure_position,
    .split = measure_split,
};

static struct netsurf_table netsurf_table = {
    .misc = &misc_table,
    .layout = &layout_table,
};

struct netsurf_table *guit = &netsurf_table;

/*--------------------------------------------------------------------------
 * Kosmos's own: what NetSurf keeps in files this system does not carry.
 *------------------------------------------------------------------------*/

/*
 * NetSurf's log, kept: its last few kilobytes of lines - where, and what -
 * in a ring that drops the oldest, read and emptied by `web.log()`. The
 * layout says why it gave up here and nowhere else, and a browser that
 * cannot say why a page would not lay out is one nobody can mend.
 */
#define LOG_KEPT 4096

static char log_ring[LOG_KEPT];
static size_t log_at, log_len;

static void log_put(const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        log_ring[(log_at + log_len) % LOG_KEPT] = s[i];

        if (log_len < LOG_KEPT) {
            log_len++;
        } else {
            log_at = (log_at + 1) % LOG_KEPT;
        }
    }
}

void nslog_log(const char *file, const char *func, int ln,
               const char *format, ...)
{
    char line[256];
    const char *name = strrchr(file, '/');
    va_list ap;
    int n;

    n = snprintf(line, sizeof(line), "%s:%d %s: ", name ? name + 1 : file,
                 ln, func);

    if (n > 0 && (size_t)n < sizeof(line)) {
        va_start(ap, format);
        (void)vsnprintf(line + n, sizeof(line) - (size_t)n, format, ap);
        va_end(ap);
    }

    log_put(line, strlen(line));
    log_put("\n", 1);
}

int web_netsurf_log(lua_State *L)
{
    luaL_Buffer b;
    size_t i;

    luaL_buffinit(L, &b);

    for (i = 0; i < log_len; i++) {
        luaL_addchar(&b, log_ring[(log_at + i) % LOG_KEPT]);
    }

    luaL_pushresult(&b);
    log_at = log_len = 0;
    return 1;
}

/* Its filter, which the option table sets when options change: there is
 * nothing to filter in a log that says nothing. */
nserror nslog_set_filter_by_options(void)
{
    return NSERROR_OK;
}

/* No translations: a message is its own key. */
const char *messages_get(const char *key)
{
    return key;
}

/* No translations of errors either: the number, which `utils/errors.h`
 * names. */
const char *messages_get_errorcode(nserror code)
{
    static char said[48];

    snprintf(said, sizeof(said), "NetSurf's error %d (utils/errors.h)",
             (int)code);
    return said;
}

/* Runs of white space as one space - `squash_whitespace` in NetSurf's
 * `utils.c`, which is otherwise POSIX stand-ins. */
char *squash_whitespace(const char *s)
{
    size_t i = 0, j = 0;
    char *out = malloc(strlen(s) + 1);

    if (out == NULL) {
        return NULL;
    }

    while (s[i] != '\0') {
        if (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t') {
            out[j++] = ' ';

            while (s[i] == ' ' || s[i] == '\n' || s[i] == '\r'
                   || s[i] == '\t') {
                i++;
            }
        } else {
            out[j++] = s[i++];
        }
    }

    out[j] = '\0';
    return out;
}

/* Every space and tab as a non-breaking space, U+00A0 in UTF-8. */
char *cnv_space2nbsp(const char *s)
{
    size_t spaces = 0, i, j = 0;
    char *out;

    for (i = 0; s[i] != '\0'; i++) {
        if (s[i] == ' ' || s[i] == '\t') {
            spaces++;
        }
    }

    out = malloc(i + spaces + 1);

    if (out == NULL) {
        return NULL;
    }

    for (i = 0; s[i] != '\0'; i++) {
        if (s[i] == ' ' || s[i] == '\t') {
            out[j++] = (char)0xc2;
            out[j++] = (char)0xa0;
        } else {
            out[j++] = s[i];
        }
    }

    out[j] = '\0';
    return out;
}

/*
 * A host name for a URL: ASCII lowered, as IDNA's mapping does, and
 * anything else refused. NetSurf's `idna.c` encodes the rest as Punycode
 * through utf8proc, which is not here. Refused, NetSurf's URL parser keeps
 * the name as it was written - its own fallback - and the resolver cannot
 * look that up, so a site named in another script does not open yet.
 */
nserror idna_encode(const char *host, size_t len, char **ace_host,
                    size_t *ace_len)
{
    size_t i;
    char *out = malloc(len + 1);

    if (out == NULL) {
        return NSERROR_NOMEM;
    }

    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)host[i];

        if (c >= 0x80) {
            free(out);
            return NSERROR_BAD_URL;
        }

        out[i] = (char)((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);
    }

    out[len] = '\0';
    *ace_host = out;
    *ace_len = len;
    return NSERROR_OK;
}

nserror idna_decode(const char *ace_host, size_t ace_len, char **host,
                    size_t *host_len)
{
    char *out = malloc(ace_len + 1);

    if (out == NULL) {
        return NSERROR_NOMEM;
    }

    memcpy(out, ace_host, ace_len);
    out[ace_len] = '\0';
    *host = out;
    *host_len = ace_len;
    return NSERROR_OK;
}

/*--------------------------------------------------------------------------
 * Stand-ins: the rest of a browser, which the layout asks about and which
 * this one does not have yet.
 *------------------------------------------------------------------------*/

/* Not printing: the layout draws for a screen. */
bool html_redraw_printing = false;
int html_redraw_printing_border = 0;
int html_redraw_printing_top_cropped = 0;

/* No visited links, so every link is drawn as not visited. */
const struct url_data *urldb_get_url_data(struct nsurl *url)
{
    (void)url;
    return NULL;
}

/* No text selection, and no search highlighting. */
bool selection_highlighted(const struct selection *s, unsigned start,
                           unsigned end, unsigned *start_idx,
                           unsigned *end_idx)
{
    (void)s;
    (void)start;
    (void)end;
    (void)start_idx;
    (void)end_idx;
    return false;
}

bool content_textsearch_ishighlighted(struct textsearch_context *textsearch,
                                      unsigned start_offset,
                                      unsigned end_offset,
                                      unsigned *start_idx,
                                      unsigned *end_idx)
{
    (void)textsearch;
    (void)start_offset;
    (void)end_offset;
    (void)start_idx;
    (void)end_idx;
    return false;
}

/*
 * Objects - pictures, and what an `<object>` embeds. j1 fetches none: a
 * box keeps no object, and is laid out by the size its attributes and
 * style give it, as NetSurf lays out one still arriving. j4 fetches them.
 */
bool html_fetch_object(struct html_content *c, struct nsurl *url,
                       struct box *box, content_type permitted_types,
                       bool background)
{
    (void)c;
    (void)url;
    (void)box;
    (void)permitted_types;
    (void)background;
    return true;
}

content_type content_factory_type_from_mime_type(lwc_string *mime_type)
{
    (void)mime_type;
    return CONTENT_NONE;
}

struct nsurl *hlcache_handle_get_url(const struct hlcache_handle *handle)
{
    (void)handle;
    return NULL;
}

bool content_can_reformat(struct hlcache_handle *h)
{
    (void)h;
    return false;
}

int content_get_available_width(struct hlcache_handle *h)
{
    (void)h;
    return 0;
}

int content_get_width(struct hlcache_handle *h)
{
    (void)h;
    return 0;
}

int content_get_height(struct hlcache_handle *h)
{
    (void)h;
    return 0;
}

bool content_get_opaque(struct hlcache_handle *h)
{
    (void)h;
    return false;
}

content_type content_get_type(struct hlcache_handle *h)
{
    (void)h;
    return CONTENT_NONE;
}

bool content_redraw(struct hlcache_handle *h, struct content_redraw_data *data,
                    const struct rect *clip, const struct redraw_context *ctx)
{
    (void)h;
    (void)data;
    (void)clip;
    (void)ctx;
    return true;
}

void content_reformat(struct hlcache_handle *h, bool background, int width,
                      int height)
{
    (void)h;
    (void)background;
    (void)width;
    (void)height;
}

struct box *html_get_box_tree(struct hlcache_handle *h)
{
    (void)h;
    return NULL;
}

/* Frames and iframes: no browser windows inside the page. */
bool browser_window_redraw(struct browser_window *bw, int x, int y,
                           const struct rect *clip,
                           const struct redraw_context *ctx)
{
    (void)bw;
    (void)x;
    (void)y;
    (void)clip;
    (void)ctx;
    return true;
}

void browser_window_reformat(struct browser_window *bw, bool background,
                             int width, int height)
{
    (void)bw;
    (void)background;
    (void)width;
    (void)height;
}

void browser_window_set_dimensions(struct browser_window *bw, int width,
                                   int height)
{
    (void)bw;
    (void)width;
    (void)height;
}

void browser_window_set_position(struct browser_window *bw, int x, int y)
{
    (void)bw;
    (void)x;
    (void)y;
}

/*
 * Scrollbars inside a page, for a box whose `overflow` scrolls. Made, so
 * the layout keeps its own account of what it asked for, and never moved
 * or drawn: the box shows its content from the top, as a browser whose
 * scrollbar nobody touches would.
 */
struct scrollbar {
    void *client_data;
};

nserror scrollbar_create(bool horizontal, int length, int full_size,
                         int visible_size, void *client_data,
                         scrollbar_client_callback client_callback,
                         struct scrollbar **s)
{
    struct scrollbar *bar = calloc(1, sizeof(*bar));

    (void)horizontal;
    (void)length;
    (void)full_size;
    (void)visible_size;
    (void)client_callback;

    if (bar == NULL) {
        return NSERROR_NOMEM;
    }

    bar->client_data = client_data;
    *s = bar;
    return NSERROR_OK;
}

void scrollbar_destroy(struct scrollbar *s)
{
    free(s);
}

void *scrollbar_get_data(struct scrollbar *s)
{
    return s->client_data;
}

int scrollbar_get_offset(struct scrollbar *s)
{
    (void)s;
    return 0;
}

void scrollbar_make_pair(struct scrollbar *horizontal,
                         struct scrollbar *vertical)
{
    (void)horizontal;
    (void)vertical;
}

nserror scrollbar_redraw(struct scrollbar *s, int x, int y,
                         const struct rect *clip, float scale,
                         const struct redraw_context *ctx)
{
    (void)s;
    (void)x;
    (void)y;
    (void)clip;
    (void)scale;
    (void)ctx;
    return NSERROR_OK;
}

void scrollbar_set_extents(struct scrollbar *s, int length, int visible_size,
                           int full_size)
{
    (void)s;
    (void)length;
    (void)visible_size;
    (void)full_size;
}

void html_overflow_scroll_callback(void *client_data,
                                   struct scrollbar_msg_data *scrollbar_data)
{
    (void)client_data;
    (void)scrollbar_data;
}

/*
 * Forms: each field a control, so the layout can size and draw it - a text
 * field's value, a button's label, a select's chosen option - and nothing
 * that edits or sends. NetSurf's `form.c` and `forms.c` are three thousand
 * lines of that, which a browser that submits nothing does not need yet;
 * without a control the layout gives up on the whole page (it did, on the
 * test page's form, found on 30 September).
 *
 * A control is made the first time an element's is asked for, and the
 * same one is handed back after that; they are kept in a list, and leave
 * it when the layout frees them.
 */
static struct form_control *controls;

/* An attribute's value, copied, or "" when there is none. */
static char *attribute_copy(dom_node *node, dom_string *name)
{
    dom_string *value = NULL;
    char *out;

    if (dom_element_get_attribute(node, name, &value) != DOM_NO_ERR
        || value == NULL) {
        return strdup("");
    }

    out = malloc(dom_string_byte_length(value) + 1);

    if (out != NULL) {
        memcpy(out, dom_string_data(value), dom_string_byte_length(value));
        out[dom_string_byte_length(value)] = '\0';
    }

    dom_string_unref(value);
    return out;
}

static bool has_attribute(dom_node *node, dom_string *name)
{
    bool has = false;

    return dom_element_has_attribute(node, name, &has) == DOM_NO_ERR && has;
}

/* What an `<input>` is, by its `type`, as NetSurf's `forms.c` reads it. */
static form_control_type input_type(dom_node *node)
{
    static const struct {
        const char *name;
        form_control_type type;
    } TYPES[] = {
        { "password", GADGET_PASSWORD }, { "file", GADGET_FILE },
        { "hidden", GADGET_HIDDEN }, { "checkbox", GADGET_CHECKBOX },
        { "radio", GADGET_RADIO }, { "submit", GADGET_SUBMIT },
        { "reset", GADGET_RESET }, { "button", GADGET_BUTTON },
        { "image", GADGET_IMAGE },
    };
    char *type = attribute_copy(node, corestring_dom_type);
    form_control_type out = GADGET_TEXTBOX;
    size_t i;

    for (i = 0; type != NULL && i < sizeof(TYPES) / sizeof(TYPES[0]); i++) {
        if (strcasecmp(type, TYPES[i].name) == 0) {
            out = TYPES[i].type;
        }
    }

    free(type);
    return out;
}

struct form_control *html_forms_get_control_for_node(struct form *forms,
                                                     dom_node *node)
{
    struct form_control *c;
    dom_string *tag = NULL;
    form_control_type type = GADGET_HIDDEN;

    (void)forms;

    for (c = controls; c != NULL; c = c->next) {
        if (c->node == node) {
            return c;
        }
    }

    if (dom_node_get_node_name(node, &tag) == DOM_NO_ERR && tag != NULL) {
        if (dom_string_caseless_lwc_isequal(tag, corestring_lwc_input)) {
            type = input_type(node);
        } else if (dom_string_caseless_lwc_isequal(tag,
                                                   corestring_lwc_button)) {
            char *kind = attribute_copy(node, corestring_dom_type);

            type = kind == NULL ? GADGET_SUBMIT
                   : strcasecmp(kind, "reset") == 0 ? GADGET_RESET
                   : strcasecmp(kind, "button") == 0 ? GADGET_BUTTON
                   : GADGET_SUBMIT;
            free(kind);
        } else if (dom_string_caseless_lwc_isequal(tag,
                                                   corestring_lwc_textarea)) {
            type = GADGET_TEXTAREA;
        } else if (dom_string_caseless_lwc_isequal(tag,
                                                   corestring_lwc_select)) {
            type = GADGET_SELECT;
        }

        dom_string_unref(tag);
    }

    c = calloc(1, sizeof(*c));

    if (c == NULL) {
        return NULL;
    }

    c->node = node;
    c->type = type;
    c->name = attribute_copy(node, corestring_dom_name);
    c->value = attribute_copy(node, corestring_dom_value);
    c->initial_value = c->value != NULL ? strdup(c->value) : NULL;
    c->selected = has_attribute(node, dom_checked);
    c->data.select.multiple = has_attribute(node, dom_multiple);

    if (c->name == NULL || c->value == NULL || c->initial_value == NULL) {
        form_free_control(c);
        return NULL;
    }

    c->next = controls;
    controls = c;
    return c;
}

/* An option of a select, its strings the control's from now on - as
 * NetSurf's own takes them. */
bool form_add_option(struct form_control *control, char *value, char *text,
                     bool selected, void *node)
{
    struct form_option *o = calloc(1, sizeof(*o));

    if (o == NULL) {
        return false;
    }

    o->node = node;
    o->value = value;
    o->text = text;
    o->selected = o->initial_selected = selected;

    if (control->data.select.last_item != NULL) {
        control->data.select.last_item->next = o;
    } else {
        control->data.select.items = o;
    }

    control->data.select.last_item = o;
    control->data.select.num_items++;

    if (selected) {
        control->data.select.num_selected++;
        control->data.select.current = o;
    }

    return true;
}

void form_free_control(struct form_control *control)
{
    struct form_control **link;
    struct form_option *o, *next;

    if (control == NULL) {
        return;
    }

    for (link = &controls; *link != NULL; link = &(*link)->next) {
        if (*link == control) {
            *link = control->next;
            break;
        }
    }

    for (o = control->data.select.items; o != NULL; o = next) {
        next = o->next;
        free(o->value);
        free(o->text);
        free(o);
    }

    free(control->name);
    free(control->value);
    free(control->initial_value);
    free(control);
}

bool form_clip_inside_select_menu(struct form_control *control, float scale,
                                  const struct rect *clip)
{
    (void)control;
    (void)scale;
    (void)clip;
    return false;
}

bool form_redraw_select_menu(struct form_control *control, int x, int y,
                             float scale, const struct rect *clip,
                             const struct redraw_context *ctx)
{
    (void)control;
    (void)x;
    (void)y;
    (void)scale;
    (void)clip;
    (void)ctx;
    return true;
}

bool box_textarea_create_textarea(struct html_content *html, struct box *box,
                                  struct dom_node *node)
{
    (void)html;
    (void)box;
    (void)node;
    return true;
}

void textarea_set_layout(struct textarea *ta, const plot_font_style_t *fstyle,
                         int width, int height, int top, int right,
                         int bottom, int left)
{
    (void)ta;
    (void)fstyle;
    (void)width;
    (void)height;
    (void)top;
    (void)right;
    (void)bottom;
    (void)left;
}

void textarea_redraw(struct textarea *ta, int x, int y, colour bg, float scale,
                     const struct rect *clip, const struct redraw_context *ctx)
{
    (void)ta;
    (void)x;
    (void)y;
    (void)bg;
    (void)scale;
    (void)clip;
    (void)ctx;
}

/*--------------------------------------------------------------------------
 * The platform's: plotting (`roadmap.md` 6zz j2).
 *
 * NetSurf draws a page through a table of eleven operations; this is the
 * table, on a `gfx` surface. Coordinates arrive in the surface's own - the
 * caller asks for the page shifted up by where the band starts - and every
 * operation is clipped to the rectangle `clip` last set, which is how a box
 * with `overflow: hidden` keeps what spills out of it.
 *
 * Colours arrive as NetSurf's 0xXXBBGGRR and go to `gfx` as 0xAARRGGBB.
 * `path` - SVG's and a canvas's - draws nothing yet, and says so by being
 * the one that does nothing.
 *------------------------------------------------------------------------*/

struct paint {
    struct surface *s;
    int width, height;          /* the surface's */
    struct rect clip;           /* the last `clip`, within the surface */
};

static uint32_t argb(colour c)
{
    return 0xff000000u | ((c & 0xffu) << 16) | (c & 0xff00u)
           | ((c >> 16) & 0xffu);
}

/* A filled rectangle, clipped: [x0, x1) by [y0, y1). */
static void fill(const struct paint *p, int x0, int y0, int x1, int y1,
                 uint32_t ink)
{
    if (x0 < p->clip.x0) x0 = p->clip.x0;
    if (y0 < p->clip.y0) y0 = p->clip.y0;
    if (x1 > p->clip.x1) x1 = p->clip.x1;
    if (y1 > p->clip.y1) y1 = p->clip.y1;

    if (x0 < x1 && y0 < y1) {
        gfx_draw_fill(p->s, x0, y0, x1 - x0, y1 - y0, ink);
    }
}

static nserror plot_clip(const struct redraw_context *ctx,
                         const struct rect *clip)
{
    struct paint *p = ctx->priv;

    p->clip.x0 = clip->x0 < 0 ? 0 : clip->x0;
    p->clip.y0 = clip->y0 < 0 ? 0 : clip->y0;
    p->clip.x1 = clip->x1 > p->width ? p->width : clip->x1;
    p->clip.y1 = clip->y1 > p->height ? p->height : clip->y1;

    return NSERROR_OK;
}

static int stroke_width(const plot_style_t *style)
{
    int w = plot_style_fixed_to_int(style->stroke_width);

    return w < 1 ? 1 : w;
}

static nserror plot_rectangle(const struct redraw_context *ctx,
                              const plot_style_t *style,
                              const struct rect *r)
{
    const struct paint *p = ctx->priv;

    if (style->fill_type != PLOT_OP_TYPE_NONE) {
        fill(p, r->x0, r->y0, r->x1, r->y1, argb(style->fill_colour));
    }

    if (style->stroke_type != PLOT_OP_TYPE_NONE) {
        int w = stroke_width(style);
        uint32_t ink = argb(style->stroke_colour);

        fill(p, r->x0, r->y0, r->x1, r->y0 + w, ink);
        fill(p, r->x0, r->y1 - w, r->x1, r->y1, ink);
        fill(p, r->x0, r->y0, r->x0 + w, r->y1, ink);
        fill(p, r->x1 - w, r->y0, r->x1, r->y1, ink);
    }

    return NSERROR_OK;
}

/* A line: straight ones as the rectangle they are, any other a pixel at a
 * time - borders and rules are straight, and are nearly all of them. */
static nserror plot_line(const struct redraw_context *ctx,
                         const plot_style_t *style, const struct rect *l)
{
    const struct paint *p = ctx->priv;
    int w = stroke_width(style);
    uint32_t ink = argb(style->stroke_colour);
    int x0 = l->x0, y0 = l->y0, x1 = l->x1, y1 = l->y1;
    int dx, dy, sx, sy, err;

    if (y0 == y1) {
        fill(p, x0 < x1 ? x0 : x1, y0 - w / 2, (x0 < x1 ? x1 : x0) + 1,
             y0 - w / 2 + w, ink);
        return NSERROR_OK;
    }

    if (x0 == x1) {
        fill(p, x0 - w / 2, y0 < y1 ? y0 : y1, x0 - w / 2 + w,
             (y0 < y1 ? y1 : y0) + 1, ink);
        return NSERROR_OK;
    }

    dx = x1 > x0 ? x1 - x0 : x0 - x1;
    dy = y1 > y0 ? y0 - y1 : y1 - y0;
    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;
    err = dx + dy;

    for (;;) {
        int e2 = 2 * err;

        fill(p, x0, y0, x0 + 1, y0 + 1, ink);

        if (x0 == x1 && y0 == y1) {
            break;
        }

        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }

    return NSERROR_OK;
}

/*
 * A polygon, filled by scanlines: each row crossed with every edge, the
 * crossings sorted, and the spans between pairs filled. A border whose
 * sides differ in colour is four of these, and a list's square bullet one.
 */
static nserror plot_polygon(const struct redraw_context *ctx,
                            const plot_style_t *style, const int *pts,
                            unsigned int n)
{
    const struct paint *p = ctx->priv;
    uint32_t ink = argb(style->fill_colour);
    int top = pts[1], bottom = pts[1], y;
    unsigned i;

    if (n < 3 || style->fill_type == PLOT_OP_TYPE_NONE) {
        return NSERROR_OK;
    }

    for (i = 1; i < n; i++) {
        if (pts[2 * i + 1] < top) top = pts[2 * i + 1];
        if (pts[2 * i + 1] > bottom) bottom = pts[2 * i + 1];
    }

    if (top < p->clip.y0) top = p->clip.y0;
    if (bottom > p->clip.y1) bottom = p->clip.y1;

    for (y = top; y < bottom; y++) {
        int cross[16];
        unsigned k = 0, a, b;

        for (i = 0; i < n && k < 16; i++) {
            int xa = pts[2 * i], ya = pts[2 * i + 1];
            int xb = pts[2 * ((i + 1) % n)], yb = pts[2 * ((i + 1) % n) + 1];

            if ((ya <= y && yb > y) || (yb <= y && ya > y)) {
                cross[k++] = xa + (y - ya) * (xb - xa) / (yb - ya);
            }
        }

        for (a = 1; a < k; a++) {
            for (b = a; b > 0 && cross[b - 1] > cross[b]; b--) {
                int t = cross[b];

                cross[b] = cross[b - 1];
                cross[b - 1] = t;
            }
        }

        for (a = 0; a + 1 < k; a += 2) {
            fill(p, cross[a], y, cross[a + 1], y + 1, ink);
        }
    }

    return NSERROR_OK;
}

/* A disc, a list's round bullet: each row's span from the circle. */
static nserror plot_disc(const struct redraw_context *ctx,
                         const plot_style_t *style, int x, int y, int radius)
{
    const struct paint *p = ctx->priv;
    int dy;

    for (dy = -radius; dy <= radius; dy++) {
        int dx = 0;

        while ((dx + 1) * (dx + 1) + dy * dy <= radius * radius) {
            dx++;
        }

        if (style->fill_type != PLOT_OP_TYPE_NONE) {
            fill(p, x - dx, y + dy, x + dx + 1, y + dy + 1,
                 argb(style->fill_colour));
        } else if (style->stroke_type != PLOT_OP_TYPE_NONE) {
            fill(p, x - dx, y + dy, x - dx + 1, y + dy + 1,
                 argb(style->stroke_colour));
            fill(p, x + dx, y + dy, x + dx + 1, y + dy + 1,
                 argb(style->stroke_colour));
        }
    }

    return NSERROR_OK;
}

/* An arc: a circle's outline, only where a list's `circle` bullet asks for
 * one, drawn whole - NetSurf asks for the whole of it there. */
static nserror plot_arc(const struct redraw_context *ctx,
                        const plot_style_t *style, int x, int y, int radius,
                        int angle1, int angle2)
{
    plot_style_t outline = *style;

    (void)angle1;
    (void)angle2;
    outline.fill_type = PLOT_OP_TYPE_NONE;
    if (outline.stroke_type == PLOT_OP_TYPE_NONE) {
        outline.stroke_type = PLOT_OP_TYPE_SOLID;
        outline.stroke_colour = style->fill_colour;
    }

    return plot_disc(ctx, &outline, x, y, radius);
}

static nserror plot_path(const struct redraw_context *ctx,
                         const plot_style_t *style, const float *pts,
                         unsigned int n, const float transform[6])
{
    (void)ctx;
    (void)style;
    (void)pts;
    (void)n;
    (void)transform;
    return NSERROR_OK;
}

/* Pictures arrive with 6zz j4, as the objects the layout sizes. */
static nserror plot_bitmap(const struct redraw_context *ctx,
                           struct bitmap *bitmap, int x, int y, int width,
                           int height, colour bg, bitmap_flags_t flags)
{
    (void)ctx;
    (void)bitmap;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)bg;
    (void)flags;
    return NSERROR_OK;
}

/*
 * Text, on its baseline, where NetSurf puts it; `gfx` takes a line's top,
 * which is the baseline less the face's ascent. A run wholly outside the
 * clip is not drawn at all - `gfx` clips to the surface and not to a box.
 */
static nserror plot_text(const struct redraw_context *ctx,
                         const plot_font_style_t *fstyle, int x, int y,
                         const char *text, size_t length)
{
    const struct paint *p = ctx->priv;
    int face = face_for(fstyle);
    int top = y - gfx_draw_ascent(face);

    if (top >= p->clip.y1 || top + gfx_draw_height(face) <= p->clip.y0
        || x >= p->clip.x1) {
        return NSERROR_OK;
    }

    gfx_draw_text(p->s, face, x, top, text, length,
                  argb(fstyle->foreground), NULL);
    return NSERROR_OK;
}

static const struct plotter_table plotters = {
    .clip = plot_clip,
    .arc = plot_arc,
    .disc = plot_disc,
    .line = plot_line,
    .rectangle = plot_rectangle,
    .polygon = plot_polygon,
    .path = plot_path,
    .bitmap = plot_bitmap,
    .text = plot_text,
    .option_knockout = false,
};

/*--------------------------------------------------------------------------
 * A document, laid out and drawn by NetSurf (`roadmap.md` 6zz j3).
 *
 * What NetSurf's `html.c` does between a parsed document and a page on the
 * screen, without the fetching, the scripts and the browser window around
 * it: a selection context with NetSurf's own default stylesheet (and its
 * quirks one, for a page in quirks mode) and the page's `<style>`s, the
 * box tree built from the DOM, laid out at a width, and drawn a band at a
 * time through the plotters above.
 *------------------------------------------------------------------------*/

/* 96 dots to the inch, the reference CSS pixels are defined against;
 * NetSurf's `css.c` says 90 and lets a front end change it. */
css_fixed nscss_screen_dpi = F_96;

static css_stylesheet *ua_default, *ua_quirks;

/* A stylesheet from text, its `url()`s resolved against `url` by NetSurf's
 * own resolver. An `@import` is not fetched yet (6zz j4). */
static css_stylesheet *sheet_of(const char *text, size_t len,
                                const char *url, bool quirks)
{
    css_stylesheet_params params;
    css_stylesheet *sheet = NULL;
    css_error e;

    memset(&params, 0, sizeof(params));
    params.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    params.level = CSS_LEVEL_DEFAULT;
    params.url = url != NULL ? url : "";
    params.allow_quirks = quirks;
    params.resolve = nscss_resolve_url;

    if (css_stylesheet_create(&params, &sheet) != CSS_OK) {
        return NULL;
    }

    e = css_stylesheet_append_data(sheet, (const uint8_t *)text, len);

    if ((e != CSS_OK && e != CSS_NEEDDATA)
        || ((e = css_stylesheet_data_done(sheet)) != CSS_OK
            && e != CSS_IMPORTS_PENDING)) {
        css_stylesheet_destroy(sheet);
        return NULL;
    }

    return sheet;
}

/* NetSurf's options at their defaults, but for the text size: 12 point,
 * which is 16 pixels, what every browser starts from. */
static nserror option_defaults(struct nsoption_s *defaults)
{
    (void)defaults;
    nsoption_set_int(font_size, 120);
    return NSERROR_OK;
}

int web_netsurf_setup(lua_State *L)
{
    size_t dlen = 0, qlen = 0;
    const char *d = luaL_checklstring(L, 1, &dlen);
    const char *q = luaL_optlstring(L, 2, "", &qlen);

    if (!netsurf_ready()) {
        return luaL_error(L, "NetSurf's names could not be made");
    }

    if (nsoptions == NULL && nsoption_init(option_defaults, NULL, NULL)
                             != NSERROR_OK) {
        return luaL_error(L, "NetSurf's options could not be made");
    }

    if (ua_default == NULL) {
        ua_default = sheet_of(d, dlen, "resource:default.css", false);
    }

    if (ua_quirks == NULL && qlen > 0) {
        ua_quirks = sheet_of(q, qlen, "resource:quirks.css", true);
    }

    lua_pushboolean(L, ua_default != NULL);
    return 1;
}

struct web_ns_doc {
    html_content     html;          /* first: NetSurf casts a content to it */
    css_stylesheet **sheets;        /* the page's own, in order */
    size_t           nsheets;
    bool             built;
    bool             converted;     /* the box tree was made */
    const char      *why;           /* why the last layout failed */
};

/* The box tree finished: NetSurf says whether it was made. The document is
 * the one `web_ns_layout` is waiting on, which is the only one there is
 * while the queue is drained. */
static struct web_ns_doc *converting;

static void converted(html_content *c, bool success)
{
    (void)c;

    if (converting != NULL) {
        converting->converted = success;
    }
}

const char *web_ns_why(struct web_ns_doc *d)
{
    return d->why != NULL ? d->why : "NetSurf could not lay this page out";
}

/* Is a `<style media="...">` for a screen? No attribute, or one naming
 * `screen` or `all`: a print sheet is not this page's. */
static bool for_screen(dom_node *node)
{
    dom_string *media = NULL;
    bool yes = true;

    if (dom_element_get_attribute(node, corestring_dom_media, &media)
        == DOM_NO_ERR && media != NULL) {
        const char *m = dom_string_data(media);
        size_t n = dom_string_byte_length(media), i;

        yes = false;

        for (i = 0; i + 3 <= n; i++) {
            if (strncasecmp(m + i, "screen", n - i < 6 ? n - i : 6) == 0
                || strncasecmp(m + i, "all", 3) == 0) {
                yes = true;
                break;
            }
        }

        dom_string_unref(media);
    }

    return yes;
}

/* Every `<style>` of the page, in order, as author sheets. */
static void page_sheets(struct web_ns_doc *d, const char *base)
{
    dom_nodelist *list = NULL;
    uint32_t n = 0, i;

    if (dom_document_get_elements_by_tag_name(d->html.document,
            corestring_dom_style, &list) != DOM_NO_ERR || list == NULL) {
        return;
    }

    (void)dom_nodelist_get_length(list, &n);
    d->sheets = calloc(n ? n : 1, sizeof(*d->sheets));

    for (i = 0; d->sheets != NULL && i < n; i++) {
        dom_node *node = NULL;
        dom_string *text = NULL;

        if (dom_nodelist_item(list, i, &node) != DOM_NO_ERR || node == NULL) {
            continue;
        }

        if (for_screen(node)
            && dom_node_get_text_content(node, &text) == DOM_NO_ERR
            && text != NULL) {
            css_stylesheet *sheet = sheet_of(dom_string_data(text),
                    dom_string_byte_length(text), base,
                    d->html.quirks != DOM_DOCUMENT_QUIRKS_MODE_NONE);

            if (sheet != NULL) {
                d->sheets[d->nsheets++] = sheet;
            }

            dom_string_unref(text);
        }

        dom_node_unref(node);
    }

    dom_nodelist_unref(list);
}

struct web_ns_doc *web_ns_open(void *document, const char *base)
{
    struct web_ns_doc *d;
    html_content *h;
    size_t i;

    if (!netsurf_ready() || ua_default == NULL) {
        return NULL;
    }

    d = calloc(1, sizeof(*d));

    if (d == NULL) {
        return NULL;
    }

    h = &d->html;
    h->document = (dom_document *)dom_node_ref((dom_node *)document);
    (void)dom_document_get_quirks_mode(h->document, &h->quirks);

    if (base == NULL || nsurl_create(base, &h->base_url) != NSERROR_OK) {
        (void)nsurl_create("about:blank", &h->base_url);
    }

    h->background_colour = NS_TRANSPARENT;
    h->media.type = CSS_MEDIA_SCREEN;
    h->font_func = &layout_table;
    h->bctx = talloc_zero(NULL, int);
    h->unit_len_ctx.device_dpi = nscss_screen_dpi;
    h->unit_len_ctx.font_size_default = INTTOFIX(16);
    h->unit_len_ctx.font_size_minimum = INTTOFIX(6);

    if (lwc_intern_string("*", 1, &h->universal) != lwc_error_ok
        || h->bctx == NULL
        || css_select_ctx_create(&h->select_ctx) != CSS_OK) {
        web_ns_close(d);
        return NULL;
    }

    (void)css_select_ctx_append_sheet(h->select_ctx, ua_default,
                                      CSS_ORIGIN_UA, NULL);

    if (ua_quirks != NULL && h->quirks != DOM_DOCUMENT_QUIRKS_MODE_NONE) {
        (void)css_select_ctx_append_sheet(h->select_ctx, ua_quirks,
                                          CSS_ORIGIN_UA, NULL);
    }

    page_sheets(d, base);

    for (i = 0; i < d->nsheets; i++) {
        (void)css_select_ctx_append_sheet(h->select_ctx, d->sheets[i],
                                          CSS_ORIGIN_AUTHOR, NULL);
    }

    return d;
}

/* Laid out at `width`; the page's whole height, or -1. The box tree is
 * built the first time, and a new width lays the same tree out again. */
int web_ns_layout(struct web_ns_doc *d, lua_State *L, int width, int height)
{
    html_content *h = &d->html;
    struct box *top;
    int tall;

    faces_L = L;
    d->why = NULL;

    if (!d->built) {
        dom_node *root = NULL;
        nserror e;

        d->built = true;

        if (dom_document_get_document_element(h->document, (void *)&root)
            != DOM_NO_ERR || root == NULL) {
            d->why = "the document has no root element";
            faces_L = NULL;
            return -1;
        }

        h->media.width = INTTOFIX(width);
        h->media.height = INTTOFIX(height);
        h->unit_len_ctx.viewport_width = INTTOFIX(width);
        h->unit_len_ctx.viewport_height = INTTOFIX(height);

        converting = d;
        e = dom_to_box(root, h, converted, &h->box_conversion_context);

        if (e == NSERROR_OK) {
            web_netsurf_run();
        }

        converting = NULL;
        dom_node_unref(root);

        if (e != NSERROR_OK) {
            d->why = messages_get_errorcode(e);
        } else if (!d->converted) {
            d->why = "NetSurf could not make the box tree";
        }
    }

    top = h->layout;

    if (top == NULL) {
        if (d->why == NULL) {
            d->why = "NetSurf made no box tree";
        }

        faces_L = NULL;
        return -1;
    }

    h->unit_len_ctx.viewport_width = INTTOFIX(width);
    h->unit_len_ctx.viewport_height = INTTOFIX(height);
    h->unit_len_ctx.root_style = top->style;

    if (!layout_document(h, width, height)) {
        d->why = "NetSurf's layout ran out of memory";
        faces_L = NULL;
        return -1;
    }

    /* The margin box, or further where something overflows it - as
     * NetSurf's `html_reformat` measures a page. */
    tall = top->y + top->padding[TOP] + top->height + top->padding[BOTTOM]
           + top->border[BOTTOM].width + top->margin[BOTTOM];

    if (tall < top->y + top->descendant_y1) {
        tall = top->y + top->descendant_y1;
    }

    h->base.width = width;
    h->base.height = tall;
    faces_L = NULL;

    return tall;
}

/* The band of the page starting `from` rows down, drawn into `s`. */
void web_ns_paint(struct web_ns_doc *d, lua_State *L, struct surface *s,
                  int width, int height, long from)
{
    struct paint p;
    struct redraw_context ctx;
    struct content_redraw_data data;
    struct rect clip = { 0, 0, width, height };

    if (d->html.layout == NULL) {
        return;
    }

    memset(&p, 0, sizeof(p));
    p.s = s;
    p.width = width;
    p.height = height;
    p.clip = clip;

    memset(&ctx, 0, sizeof(ctx));
    ctx.interactive = false;
    ctx.background_images = true;
    ctx.plot = &plotters;
    ctx.priv = &p;

    memset(&data, 0, sizeof(data));
    data.x = 0;
    data.y = (int)-from;
    data.width = width;
    data.height = height;
    data.background_colour = 0xffffff;
    data.scale = 1.0f;

    faces_L = L;
    (void)html_redraw(&d->html.base, &data, &clip, &ctx);
    faces_L = NULL;
}

/* The address of the link under a point of the page, or NULL: the deepest
 * box there with one, as NetSurf's own pointer handling finds it. */
const char *web_ns_link_at(struct web_ns_doc *d, int x, int y)
{
    struct box *box = d->html.layout;
    const char *href = NULL;
    int bx = 0, by = 0;

    if (box == NULL) {
        return NULL;
    }

    while ((box = box_at_point(&d->html.unit_len_ctx, box, x, y, &bx, &by))
           != NULL) {
        if (box->href != NULL) {
            href = nsurl_access(box->href);
        }
    }

    return href;
}

void web_ns_close(struct web_ns_doc *d)
{
    html_content *h;
    size_t i;

    if (d == NULL) {
        return;
    }

    h = &d->html;

    if (h->select_ctx != NULL) {
        css_select_ctx_destroy(h->select_ctx);
    }

    for (i = 0; i < d->nsheets; i++) {
        css_stylesheet_destroy(d->sheets[i]);
    }

    free(d->sheets);

    if (h->bctx != NULL) {
        talloc_free(h->bctx);
    }

    if (h->universal != NULL) {
        lwc_string_unref(h->universal);
    }

    if (h->base_url != NULL) {
        nsurl_unref(h->base_url);
    }

    if (h->document != NULL) {
        dom_node_unref((dom_node *)h->document);
    }

    free(d);
}
