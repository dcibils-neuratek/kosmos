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
 *   - **Stand-ins**, for what a browser without scripts does not do yet:
 *     scrollbars inside a page, the page's text selection, visited links,
 *     a select's menu. Each does nothing and says so, so that what the
 *     layout does with its answer is what it does with a browser that has
 *     none of those - which is a case its authors handle.
 *   - **Forms**, since j6: NetSurf's own form and text-area code, and the
 *     browser's half of it here - the caret, what to draw again, what a
 *     click and a key do to a field, and a form sent kept for Lua to fetch.
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
#include <kosmos.h>

#include <dom/dom.h>
#include <parserutils/charset/codec.h>
#include <parserutils/charset/utf8.h>
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
#include "utils/utf8.h"
#include "netsurf/browser_window.h"
#include "netsurf/clipboard.h"
#include "netsurf/keypress.h"
#include "netsurf/mouse.h"
#include "netsurf/content.h"
#include "netsurf/content_type.h"
#include "netsurf/layout.h"
#include "netsurf/misc.h"
#include "netsurf/plot_style.h"
#include "netsurf/plotters.h"
#include "netsurf/url_db.h"
#include "content/content_factory.h"
#include "content/fetch.h"
#include "content/textsearch.h"
#include "desktop/gui_internal.h"
#include "desktop/gui_table.h"
#include "desktop/print.h"
#include "desktop/scrollbar.h"
#include "desktop/system_colour.h"
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

static bool netsurf_ready(void)
{
    if (!ready && corestrings_init() == NSERROR_OK
        && css_hint_init() == NSERROR_OK) {
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
    9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 80, 96
};

/* The zoom in force, per cent (`web.zoom`, below): the text is drawn at it
 * as everything else on the page is laid out at it. */
static int zoom_pct = 100;

#define RUNGS  (sizeof(LADDER) / sizeof(LADDER[0]))
#define KINDS  6

static const char *const KIND_FILE[KINDS] = {
    "ibmplexsans", "ibmplexsans-bold", "ibmplexsans-italic",
    "ibmplexsans-bolditalic", "ibmplexmono", "ibmplexmono-bold",
};

static int faces[KINDS][RUNGS];
static bool faces_known[KINDS][RUNGS];

/*
 * A style's size in points, as pixels at the reference 96 to the inch and
 * the zoom, and the rung of the ladder at or above it. libcss gives a
 * font's size in points reckoned at 96 whatever `device_dpi` is, so the
 * zoom that lays the page out larger has to draw its words larger here -
 * the first zoom laid the page out at 150% and drew its letters at 100%.
 */
static unsigned rung_of(const plot_font_style_t *fstyle)
{
    int px = (int)(((long)fstyle->size * 96 / 72 * zoom_pct / 100
                    + PLOT_STYLE_SCALE / 2) / PLOT_STYLE_SCALE);
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

static struct gui_clipboard_table clipboard_table;

static struct netsurf_table netsurf_table = {
    .misc = &misc_table,
    .layout = &layout_table,
    .clipboard = &clipboard_table,
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

/*
 * **What a paint spent, by kind** (`roadmap.md` 6zz h): fills, text,
 * pictures and the other shapes, each timed as NetSurf asks for it - so the
 * vector unit goes where the time is, which is the order 6zz h was agreed
 * in. Two readings of the counter a call, which is nothing beside drawing.
 * What the paint spent outside them all is NetSurf walking its boxes.
 */
static struct web_ns_costs costs;

#define TIMED(kind, call)                                                  \
    do {                                                                   \
        unsigned long t0_ = kosmos_ticks();                                \
        nserror e_ = (call);                                               \
                                                                           \
        costs.kind.ticks += kosmos_ticks() - t0_;                          \
        costs.kind.calls++;                                                \
        return e_;                                                         \
    } while (0)

static nserror timed_clip(const struct redraw_context *ctx,
                          const struct rect *clip)
{
    TIMED(other, plot_clip(ctx, clip));
}

static nserror timed_arc(const struct redraw_context *ctx,
                         const plot_style_t *style, int x, int y, int radius,
                         int angle1, int angle2)
{
    TIMED(shapes, plot_arc(ctx, style, x, y, radius, angle1, angle2));
}

static nserror timed_disc(const struct redraw_context *ctx,
                          const plot_style_t *style, int x, int y,
                          int radius)
{
    TIMED(shapes, plot_disc(ctx, style, x, y, radius));
}

static nserror timed_line(const struct redraw_context *ctx,
                          const plot_style_t *style, const struct rect *line)
{
    TIMED(fills, plot_line(ctx, style, line));
}

static nserror timed_rectangle(const struct redraw_context *ctx,
                               const plot_style_t *style,
                               const struct rect *rect)
{
    TIMED(fills, plot_rectangle(ctx, style, rect));
}

static nserror timed_polygon(const struct redraw_context *ctx,
                             const plot_style_t *style, const int *p,
                             unsigned int n)
{
    TIMED(shapes, plot_polygon(ctx, style, p, n));
}

static nserror timed_path(const struct redraw_context *ctx,
                          const plot_style_t *pstyle, const float *p,
                          unsigned int n, const float transform[6])
{
    TIMED(shapes, plot_path(ctx, pstyle, p, n, transform));
}

static nserror timed_bitmap(const struct redraw_context *ctx,
                            struct bitmap *bitmap, int x, int y, int width,
                            int height, colour bg,
                            bitmap_flags_t flags)
{
    TIMED(pictures, plot_bitmap(ctx, bitmap, x, y, width, height, bg,
                                flags));
}

static nserror timed_text(const struct redraw_context *ctx,
                          const struct plot_font_style *fstyle, int x, int y,
                          const char *text, size_t length)
{
    TIMED(text, plot_text(ctx, fstyle, x, y, text, length));
}

static const struct plotter_table plotters = {
    .clip = timed_clip,
    .arc = timed_arc,
    .disc = timed_disc,
    .line = timed_line,
    .rectangle = timed_rectangle,
    .polygon = timed_polygon,
    .path = timed_path,
    .bitmap = timed_bitmap,
    .text = timed_text,
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

/*
 * **Zoom, all of the page** - Diego, 1 October: "we need a way to zoom the
 * page likke chrome does to increase or decrease sizes of all fonts, etc",
 * in four steps. How many of the screen's pixels a CSS pixel is: libcss
 * turns every length a page gives - a font's size, a margin, a border, a
 * box's width - into the screen's pixels through `device_dpi` when the page
 * is laid out, so 96 is 100% and 192 is 200%, and the page is laid out
 * again at it rather than parsed again. A picture with no size of its own
 * in the page takes its own pixels, which are scaled the same way
 * (`content_get_width`). It replaced a text size that scaled only what a
 * page leaves to the browser (d5).
 */
static css_fixed zoomed_dpi(void)
{
    return FDIV(INTTOFIX(96 * zoom_pct), INTTOFIX(100));
}

/* `web.zoom(percent)`: the zoom every page is laid out at from now on. */
int web_netsurf_zoom(lua_State *L)
{
    lua_Integer pct = luaL_checkinteger(L, 1);

    if (pct < 25 || pct > 500) {
        return luaL_error(L, "a zoom of %d%%", (int)pct);
    }

    zoom_pct = (int)pct;
    return 0;
}

int web_netsurf_setup(lua_State *L)
{
    size_t dlen = 0, qlen = 0;
    const char *d = luaL_checklstring(L, 1, &dlen);
    const char *q = luaL_optlstring(L, 2, "", &qlen);

    if (!netsurf_ready()) {
        return luaL_error(L, "NetSurf's names could not be made");
    }

    /*
     * And after the options, the system colours - CSS's `Canvas`,
     * `ButtonText` and the rest, which read them - as NetSurf's own
     * `netsurf_init` makes them; Wikipedia's stylesheet names them, and
     * the first page that did faulted in `ns_system_colour`.
     */
    if (nsoptions == NULL) {
        if (nsoption_init(option_defaults, NULL, NULL) != NSERROR_OK) {
            return luaL_error(L, "NetSurf's options could not be made");
        }

        if (ns_system_colour_init() != NSERROR_OK) {
            return luaL_error(L, "NetSurf's system colours could not be made");
        }
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

/*
 * A stylesheet of the page's, in the order the page gives it: a `<style>`'s
 * made at once, a `<link rel="stylesheet">`'s when the browser has fetched
 * it and handed the text over (`web_ns_sheet`). The cascade is made from
 * them all, in this order, at the first layout.
 */
struct page_sheet {
    css_stylesheet *sheet;          /* NULL until a link's text arrives */
    nsurl          *url;            /* a link's, joined to the page */
};

/* A picture the layout asked for (`html_fetch_object`, below): NetSurf's
 * name for a fetched content, opaque to everything vendored. */
struct hlcache_handle {
    struct box            *box;
    nsurl                 *url;
    bool                   background;
    int                    width, height;   /* natural, once arrived */
    struct surface        *pic;             /* NULL until it has */
    int                    ref;             /* the registry's hold on it */
    struct hlcache_handle *next;
};

struct web_ns_doc {
    html_content       html;        /* first: NetSurf casts a content to it */
    struct page_sheet *sheets;
    size_t             nsheets, sheets_room;
    struct hlcache_handle *objects; /* the pictures asked for, newest first */
    size_t             nobjects;
    bool               built;
    int                laid_zoom;   /* the zoom it was last laid out at */
    bool               converted;   /* the box tree was made */
    const char        *why;         /* why the last layout failed */

    /* Forms (`roadmap.md` 6zz j6). */
    struct box        *focus;       /* the field with the caret, or NULL */
    int                caret_x, caret_y, caret_h;   /* on the page */
    struct rect        dirty;       /* what changed since it was asked */
    bool               is_dirty;
    char              *sent_url;    /* a form sent, for the browser to get */
    char              *sent_body;   /* what it POSTs, or NULL for a GET */
    char               sent_type[96];               /* the body's type */
    unsigned char      pending[4];  /* a character arriving byte by byte */
    int                have, need;

    struct web_ns_costs costs;      /* what the last paint spent */
};

const struct web_ns_costs *web_ns_costs(struct web_ns_doc *d)
{
    return &d->costs;
}

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

/*--------------------------------------------------------------------------
 * Forms (`roadmap.md` 6zz j6).
 *
 * NetSurf's own `forms.c`, `form.c`, `box_textarea.c` and `textarea.c` keep
 * a page's forms, edit a field's text, move its caret and encode what is
 * sent. What they ask of the browser around them is here: where the caret
 * is, what to draw again, a clipboard, a few of `utf8.c`'s helpers, and
 * what to do with a form that was sent - which is to keep it for the
 * browser, whose fetching is Lua's (`browser.lua`).
 *
 * What NetSurf's `interaction.c` does with a click and a key is done by
 * `web_ns_click` and `web_ns_key` below, for the form fields alone: its
 * other half is selection, frames, image maps and scripts, which this
 * browser does not have.
 *------------------------------------------------------------------------*/

/* A browser window for the form code to hand back. It asks whether there
 * is one before it reports a caret, and never looks inside. */
static char the_window;
#define THE_WINDOW ((struct browser_window *)(void *)&the_window)

/* The document a click or a key is being given to, which is where a form
 * it sends is kept: the window above is every document's. */
static struct web_ns_doc *acting;

static struct web_ns_doc *doc_of(html_content *h)
{
    return (struct web_ns_doc *)(void *)h;     /* `html` is its first */
}

/* `area`, in page coordinates, to be drawn again. */
static void dirty_add(struct web_ns_doc *d, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return;
    }

    if (!d->is_dirty) {
        d->dirty.x0 = x;
        d->dirty.y0 = y;
        d->dirty.x1 = x + w;
        d->dirty.y1 = y + h;
        d->is_dirty = true;
        return;
    }

    if (x < d->dirty.x0) d->dirty.x0 = x;
    if (y < d->dirty.y0) d->dirty.y0 = y;
    if (x + w > d->dirty.x1) d->dirty.x1 = x + w;
    if (y + h > d->dirty.y1) d->dirty.y1 = y + h;
}

void content__request_redraw(struct content *c, int x, int y, int width,
                             int height)
{
    dirty_add(doc_of((html_content *)(void *)c), x, y, width, height);
}

void html__redraw_a_box(html_content *htmlc, struct box *box)
{
    int x, y;

    box_coords(box, &x, &y);
    dirty_add(doc_of(htmlc), x, y,
              box->border[LEFT].width + box->padding[LEFT] + box->width
              + box->padding[RIGHT] + box->border[RIGHT].width,
              box->border[TOP].width + box->padding[TOP] + box->height
              + box->padding[BOTTOM] + box->border[BOTTOM].width);
}

struct nsurl *content_get_url(struct content *c)
{
    return ((html_content *)(void *)c)->base_url;
}

/*
 * The caret, as the text area reports it: where in its box, or hidden. A
 * field with a caret has the keyboard; a hidden one has given it up, which
 * is how a click elsewhere, Escape and Tab all end in the same state.
 */
void html_set_focus(html_content *html, html_focus_type focus_type,
                    union html_focus_owner focus_owner, bool hide_caret,
                    int x, int y, int height, const struct rect *clip)
{
    struct web_ns_doc *d = doc_of(html);
    int bx, by;

    (void)clip;

    if (d->focus != NULL) {
        dirty_add(d, d->caret_x - 1, d->caret_y, 3, d->caret_h);
    }

    html->focus_type = focus_type;
    html->focus_owner = focus_owner;

    if (focus_type != HTML_FOCUS_TEXTAREA || hide_caret) {
        if (focus_type != HTML_FOCUS_TEXTAREA
            || focus_owner.textarea == d->focus) {
            d->focus = NULL;
        }

        return;
    }

    box_coords(focus_owner.textarea, &bx, &by);
    d->focus = focus_owner.textarea;
    d->caret_x = bx + x;
    d->caret_y = by + y;
    d->caret_h = height;
    dirty_add(d, d->caret_x - 1, d->caret_y, 3, d->caret_h);
}

void html_set_drag_type(html_content *html, html_drag_type drag_type,
                        union html_drag_owner drag_owner,
                        const struct rect *rect)
{
    (void)rect;
    html->drag_type = drag_type;
    html->drag_owner = drag_owner;
}

void html_set_selection(html_content *html,
                        html_selection_type selection_type,
                        union html_selection_owner selection_owner,
                        bool read_only)
{
    html->selection_type = selection_type;
    html->selection_owner = selection_owner;
    (void)read_only;
}

void browser_window_set_drag_type(struct browser_window *bw,
                                  browser_drag_type type,
                                  const struct rect *rect)
{
    (void)bw;
    (void)type;
    (void)rect;
}

/*
 * A form sent: kept for the browser, which fetches it as it fetches
 * anything (`browser.lua`). A GET is an address with its query; a POST is
 * an address and a body, URL-encoded as NetSurf made it or multipart as it
 * is put together here.
 */
static char *multipart(const struct fetch_multipart_data *items,
                       const char *boundary)
{
    const struct fetch_multipart_data *i;
    size_t room = strlen(boundary) + 8, at = 0;
    char *out;

    for (i = items; i != NULL; i = i->next) {
        room += strlen(boundary) + strlen(i->name) + strlen(i->value)
                + (i->rawfile != NULL ? strlen(i->rawfile) : 0) + 160;
    }

    out = malloc(room);

    if (out == NULL) {
        return NULL;
    }

    for (i = items; i != NULL; i = i->next) {
        if (i->file) {
            /* No file is sent: a browser with no file chooser has none to
             * send, and says so with an empty one, as a form left empty
             * does. */
            at += (size_t)snprintf(out + at, room - at,
                "--%s\r\nContent-Disposition: form-data; name=\"%s\"; "
                "filename=\"\"\r\nContent-Type: application/octet-stream"
                "\r\n\r\n\r\n", boundary, i->name);
        } else {
            at += (size_t)snprintf(out + at, room - at,
                "--%s\r\nContent-Disposition: form-data; name=\"%s\""
                "\r\n\r\n%s\r\n", boundary, i->name, i->value);
        }
    }

    (void)snprintf(out + at, room - at, "--%s--\r\n", boundary);
    return out;
}

nserror browser_window_navigate(struct browser_window *bw, struct nsurl *url,
                                struct nsurl *referrer,
                                enum browser_window_nav_flags flags,
                                char *post_urlenc,
                                struct fetch_multipart_data *post_multipart,
                                struct hlcache_handle *parent)
{
    struct web_ns_doc *d = acting;

    (void)bw;
    (void)referrer;
    (void)flags;
    (void)parent;

    if (d == NULL) {
        return NSERROR_BAD_PARAMETER;
    }

    free(d->sent_url);
    free(d->sent_body);
    d->sent_url = strdup(nsurl_access(url));
    d->sent_body = NULL;
    d->sent_type[0] = '\0';

    if (post_urlenc != NULL) {
        d->sent_body = strdup(post_urlenc);
        (void)snprintf(d->sent_type, sizeof(d->sent_type),
                       "application/x-www-form-urlencoded");
    } else if (post_multipart != NULL) {
        static const char boundary[] = "----KosmosFormBoundary7MA4YWxk";

        d->sent_body = multipart(post_multipart, boundary);
        (void)snprintf(d->sent_type, sizeof(d->sent_type),
                       "multipart/form-data; boundary=%s", boundary);
    }

    return d->sent_url != NULL ? NSERROR_OK : NSERROR_NOMEM;
}

void fetch_multipart_data_destroy(struct fetch_multipart_data *list)
{
    struct fetch_multipart_data *next;

    for (; list != NULL; list = next) {
        next = list->next;
        free(list->name);
        free(list->value);
        free(list->rawfile);
        free(list);
    }
}

/*
 * The scrollbars a text area makes when its text outgrows it: made by the
 * stand-ins above, and these are the rest of what it asks of them. Its
 * text scrolls with its caret all the same; there is no bar to drag.
 */
void scrollbar_set(struct scrollbar *s, int value, bool bar_pos)
{
    (void)s;
    (void)value;
    (void)bar_pos;
}

bool scrollbar_scroll(struct scrollbar *s, int change)
{
    (void)s;
    (void)change;
    return false;
}

scrollbar_mouse_status scrollbar_mouse_action(struct scrollbar *s,
                                              browser_mouse_state mouse,
                                              int x, int y)
{
    (void)s;
    (void)mouse;
    (void)x;
    (void)y;
    return SCROLLBAR_MOUSE_NONE;
}

void scrollbar_mouse_drag_end(struct scrollbar *s, browser_mouse_state mouse,
                              int x, int y)
{
    (void)s;
    (void)mouse;
    (void)x;
    (void)y;
}

const char *scrollbar_mouse_status_to_message(scrollbar_mouse_status status)
{
    (void)status;
    return "";
}

/*
 * The clipboard the text area cuts, copies and pastes through - its own,
 * for now, within the browser; the desktop's is a step of its own.
 */
static char *clip;
static size_t clip_length;

static void clipboard_get(char **buffer, size_t *length)
{
    *buffer = NULL;
    *length = 0;

    if (clip != NULL && (*buffer = malloc(clip_length + 1)) != NULL) {
        memcpy(*buffer, clip, clip_length);
        (*buffer)[clip_length] = '\0';
        *length = clip_length;
    }
}

static void clipboard_set(const char *buffer, size_t length,
                          nsclipboard_styles styles[], int n_styles)
{
    (void)styles;
    (void)n_styles;

    free(clip);
    clip = malloc(length + 1);
    clip_length = 0;

    if (clip != NULL) {
        memcpy(clip, buffer, length);
        clip[length] = '\0';
        clip_length = length;
    }
}

static struct gui_clipboard_table clipboard_table = {
    .get = clipboard_get,
    .set = clipboard_set,
};

/*
 * `utils/utf8.c`'s helpers that the text area and the forms use, as NetSurf
 * writes them, over libparserutils - the file itself wants `iconv`, which
 * is not here (`README.kosmos.md`).
 */
uint32_t utf8_to_ucs4(const char *s_in, size_t l)
{
    uint32_t ucs4;
    size_t len;

    if (parserutils_charset_utf8_to_ucs4((const uint8_t *)s_in, l, &ucs4,
                                         &len) != PARSERUTILS_OK) {
        ucs4 = 0xfffd;
    }

    return ucs4;
}

size_t utf8_from_ucs4(uint32_t c, char *s)
{
    uint8_t *in = (uint8_t *)s;
    size_t len = 6;

    if (parserutils_charset_utf8_from_ucs4(c, &in, &len) != PARSERUTILS_OK) {
        s[0] = (char)0xef;
        s[1] = (char)0xbf;
        s[2] = (char)0xbd;
        return 3;
    }

    return 6 - len;
}

size_t utf8_bounded_length(const char *s, size_t l)
{
    size_t len;

    if (parserutils_charset_utf8_length((const uint8_t *)s, l, &len)
        != PARSERUTILS_OK) {
        return 0;
    }

    return len;
}

size_t utf8_length(const char *s)
{
    return utf8_bounded_length(s, strlen(s));
}

size_t utf8_prev(const char *s, size_t o)
{
    uint32_t prev = 0;

    (void)parserutils_charset_utf8_prev((const uint8_t *)s, (uint32_t)o,
                                        &prev);
    return prev;
}

size_t utf8_next(const char *s, size_t l, size_t o)
{
    uint32_t next = (uint32_t)l;

    (void)parserutils_charset_utf8_next((const uint8_t *)s, (uint32_t)l,
                                        (uint32_t)o, &next);
    return next;
}

size_t utf8_bounded_byte_length(const char *s, size_t l, size_t c)
{
    size_t len = 0;

    while (len < l && c-- > 0) {
        len = utf8_next(s, l, len);
    }

    return len;
}

/*
 * UTF-8 into the charset a form is sent in, through libparserutils' own
 * encoders - the ones its parser reads pages with, ISO-8859 and Windows'
 * code pages among them. `//TRANSLIT` on the name asks for a stand-in
 * where a character has none, which is what these do anyway: a `?`.
 */
nserror utf8_to_enc(const char *string, const char *encname, size_t len,
                    char **result)
{
    parserutils_charset_codec *codec = NULL;
    parserutils_charset_codec_optparams loose;
    char name[64];
    const char *cut = strstr(encname, "//");
    size_t n = cut != NULL ? (size_t)(cut - encname) : strlen(encname);
    size_t at = 0, room;
    uint8_t *out, *dest;
    size_t destlen;

    if (len == 0) {
        len = strlen(string);
    }

    if (n >= sizeof(name)) {
        return NSERROR_BAD_ENCODING;
    }

    memcpy(name, encname, n);
    name[n] = '\0';

    if (strcasecmp(name, "UTF-8") == 0 || strcasecmp(name, "UTF8") == 0) {
        *result = strndup(string, len);
        return *result != NULL ? NSERROR_OK : NSERROR_NOMEM;
    }

    if (parserutils_charset_codec_create(name, &codec) != PARSERUTILS_OK) {
        return NSERROR_BAD_ENCODING;
    }

    loose.error_mode.mode = PARSERUTILS_CHARSET_CODEC_ERROR_LOOSE;
    (void)parserutils_charset_codec_setopt(codec,
                                           PARSERUTILS_CHARSET_CODEC_ERROR_MODE,
                                           &loose);

    room = len * 4 + 8;
    out = malloc(room);

    if (out == NULL) {
        parserutils_charset_codec_destroy(codec);
        return NSERROR_NOMEM;
    }

    dest = out;
    destlen = room - 1;

    while (at < len) {
        uint32_t c = utf8_to_ucs4(string + at, len - at);
        uint8_t be[4] = { (uint8_t)(c >> 24), (uint8_t)(c >> 16),
                          (uint8_t)(c >> 8), (uint8_t)c };
        const uint8_t *src = be;
        size_t srclen = sizeof(be);
        size_t next = utf8_next(string, len, at);

        (void)parserutils_charset_codec_encode(codec, &src, &srclen, &dest,
                                               &destlen);
        at = next > at ? next : len;
    }

    *dest = '\0';
    parserutils_charset_codec_destroy(codec);
    *result = (char *)out;
    return NSERROR_OK;
}

/*
 * What a key is to NetSurf, from what the kit makes of it (`keys.lua`): a
 * character is itself - a byte, so a letter beyond ASCII arrives in two to
 * four and is put back together here - and every other key a small
 * negative number, less 1024 for each step of its modifiers. 0 for a key a
 * field has no use for, or a character not yet whole.
 */
static uint32_t ns_key_of(struct web_ns_doc *d, int c)
{
    int mods = (255 - c) / 1024;
    int key = c + 1024 * mods;
    bool ctrl = (mods & 4) != 0, shift = (mods & 1) != 0;

    if (c >= 0x80 && c <= 0xff) {
        if (d->need == 0 || (c & 0xc0) != 0x80) {
            d->need = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 0;
            d->have = 0;

            if (d->need == 0) {
                return 0;               /* a continuation with no start */
            }
        }

        d->pending[d->have++] = (unsigned char)c;

        if (d->have < d->need) {
            return 0;
        }

        d->need = 0;
        return utf8_to_ucs4((const char *)d->pending, (size_t)d->have);
    }

    d->need = 0;

    if (key >= 32 && key < 127 && mods == 0) {
        return (uint32_t)key;
    }

    switch (key) {
    case 8: case 127: return NS_KEY_DELETE_LEFT;
    case 9:           return shift ? NS_KEY_SHIFT_TAB : NS_KEY_TAB;
    case 10: case 13: return NS_KEY_NL;
    case 27:          return NS_KEY_ESCAPE;
    case 1:           return NS_KEY_SELECT_ALL;
    case 3:           return NS_KEY_COPY_SELECTION;
    case 21:          return NS_KEY_DELETE_LINE;
    case 22:          return NS_KEY_PASTE;
    case 24:          return NS_KEY_CUT_SELECTION;
    case -1:          return NS_KEY_UP;
    case -2:          return NS_KEY_DOWN;
    case -3:          return ctrl ? NS_KEY_WORD_RIGHT : NS_KEY_RIGHT;
    case -4:          return ctrl ? NS_KEY_WORD_LEFT : NS_KEY_LEFT;
    case -5:          return ctrl ? NS_KEY_TEXT_START : NS_KEY_LINE_START;
    case -6:          return ctrl ? NS_KEY_TEXT_END : NS_KEY_LINE_END;
    case -7:          return NS_KEY_PAGE_UP;
    case -8:          return NS_KEY_PAGE_DOWN;
    case -10:         return NS_KEY_DELETE_RIGHT;
    default:          return 0;
    }
}

bool web_ns_focused(struct web_ns_doc *d)
{
    return d->focus != NULL;
}

/* The caret taken out of its field, which gives the keyboard back. */
void web_ns_blur(struct web_ns_doc *d, lua_State *L)
{
    struct box *had = d->focus;

    if (had != NULL && had->gadget != NULL
        && had->gadget->data.text.ta != NULL) {
        faces_L = L;
        acting = d;
        textarea_set_caret(had->gadget->data.text.ta, -1);
        acting = NULL;
        faces_L = NULL;
    }

    d->focus = NULL;
}

/* A key for the field with the caret: whether it was taken. */
bool web_ns_key(struct web_ns_doc *d, lua_State *L, int key)
{
    uint32_t ns;

    if (d->focus == NULL || d->focus->gadget == NULL) {
        return false;
    }

    ns = ns_key_of(d, key);

    if (ns == 0) {
        return d->need != 0;            /* half a character is taken */
    }

    if (ns == NS_KEY_ESCAPE) {
        web_ns_blur(d, L);
        return true;
    }

    faces_L = L;
    acting = d;
    (void)box_textarea_keypress(&d->html, d->focus, ns);
    acting = NULL;
    faces_L = NULL;
    return true;
}

/*
 * A press at (x, y) on the page, for a form field there - as NetSurf's
 * `html_mouse_action` treats one: a text field takes the caret where the
 * press was, a checkbox turns over, a radio button is chosen from its
 * group, and a submit button sends its form. What it did, or NULL when
 * there is no field there - which takes the caret out of any that had it.
 */
const char *web_ns_click(struct web_ns_doc *d, lua_State *L, int x, int y)
{
    struct box *box = d->html.layout, *gadget_box = NULL;
    struct form_control *gadget;
    int bx = 0, by = 0, gx = 0, gy = 0;
    const char *did = NULL;

    if (box == NULL) {
        return NULL;
    }

    while ((box = box_at_point(&d->html.unit_len_ctx, box, x, y, &bx, &by))
           != NULL) {
        if (box->gadget != NULL) {
            gadget_box = box;
            gx = bx;
            gy = by;
        }
    }

    if (gadget_box == NULL) {
        web_ns_blur(d, L);
        return NULL;
    }

    gadget = gadget_box->gadget;

    if (d->focus != NULL && d->focus != gadget_box) {
        web_ns_blur(d, L);
    }

    faces_L = L;
    acting = d;

    switch (gadget->type) {
    case GADGET_TEXTBOX:
    case GADGET_TEXTAREA:
    case GADGET_PASSWORD:
        if (gadget->data.text.ta != NULL) {
            (void)textarea_mouse_action(gadget->data.text.ta,
                                        BROWSER_MOUSE_PRESS_1, x - gx,
                                        y - gy);
            (void)textarea_mouse_action(gadget->data.text.ta,
                                        BROWSER_MOUSE_CLICK_1, x - gx,
                                        y - gy);
            did = "field";
        }
        break;

    case GADGET_CHECKBOX:
        gadget->selected = !gadget->selected;
        (void)dom_html_input_element_set_checked(
            (dom_html_input_element *)gadget->node, gadget->selected);
        html__redraw_a_box(&d->html, gadget_box);
        did = "toggled";
        break;

    case GADGET_RADIO:
        form_radio_set(gadget);
        did = "toggled";
        break;

    case GADGET_IMAGE:
        gadget->data.image.mx = x - gx;
        gadget->data.image.my = y - gy;
        /* fall through - an image button sends its form, and where */

    case GADGET_SUBMIT:
        if (gadget->form != NULL
            && form_submit(d->html.base_url, THE_WINDOW, gadget->form,
                           gadget) == NSERROR_OK
            && d->sent_url != NULL) {
            did = "sent";
        }
        break;

    default:
        break;
    }

    acting = NULL;
    faces_L = NULL;
    return did;
}

/* A form sent, taken: its address, and its body and type when POSTed. */
bool web_ns_sent(struct web_ns_doc *d, char **url, char **body,
                 const char **type)
{
    if (d->sent_url == NULL) {
        return false;
    }

    *url = d->sent_url;
    *body = d->sent_body;
    *type = d->sent_type[0] != '\0' ? d->sent_type : NULL;
    d->sent_url = NULL;
    d->sent_body = NULL;
    return true;
}

/* What changed on the page since this was last asked, taken. */
bool web_ns_dirty(struct web_ns_doc *d, int *x, int *y, int *w, int *h)
{
    if (!d->is_dirty) {
        return false;
    }

    *x = d->dirty.x0;
    *y = d->dirty.y0;
    *w = d->dirty.x1 - d->dirty.x0;
    *h = d->dirty.y1 - d->dirty.y0;
    d->is_dirty = false;
    return true;
}

const char *web_ns_why(struct web_ns_doc *d)
{
    return d->why != NULL ? d->why : "NetSurf could not lay this page out";
}

/* Is a `<style>` or `<link>` for a screen? No `media`, or one naming
 * `screen` or `all`: a print sheet is not this page's. */
static bool for_screen(dom_node *node)
{
    dom_string *media = NULL;
    bool yes = true;

    if (dom_element_get_attribute(node, corestring_dom_media, &media)
        == DOM_NO_ERR && media != NULL) {
        const char *m = dom_string_data(media);
        size_t n = dom_string_byte_length(media), i;

        yes = n == 0;

        for (i = 0; !yes && i < n; i++) {
            if ((n - i >= 6 && strncasecmp(m + i, "screen", 6) == 0)
                || (n - i >= 3 && strncasecmp(m + i, "all", 3) == 0)) {
                yes = true;
            }
        }

        dom_string_unref(media);
    }

    return yes;
}

/* Does a `rel` name a stylesheet, and not an alternate one? Its words,
 * without regard to case. */
static bool rel_is_stylesheet(dom_node *node)
{
    dom_string *rel = NULL;
    bool sheet = false, alternate = false;

    if (dom_element_get_attribute(node, corestring_dom_rel, &rel)
        != DOM_NO_ERR || rel == NULL) {
        return false;
    }

    {
        const char *r = dom_string_data(rel);
        size_t n = dom_string_byte_length(rel), i = 0;

        while (i < n) {
            size_t start;

            while (i < n && (r[i] == ' ' || r[i] == '\t' || r[i] == '\n'
                             || r[i] == '\r' || r[i] == '\f')) {
                i++;
            }

            start = i;

            while (i < n && r[i] != ' ' && r[i] != '\t' && r[i] != '\n'
                   && r[i] != '\r' && r[i] != '\f') {
                i++;
            }

            if (i - start == 10 && strncasecmp(r + start, "stylesheet", 10) == 0) {
                sheet = true;
            } else if (i - start == 9
                       && strncasecmp(r + start, "alternate", 9) == 0) {
                alternate = true;
            }
        }
    }

    dom_string_unref(rel);
    return sheet && !alternate;
}

static struct page_sheet *more_sheet(struct web_ns_doc *d)
{
    if (d->nsheets == d->sheets_room) {
        size_t room = d->sheets_room ? d->sheets_room * 2 : 8;
        struct page_sheet *grown = realloc(d->sheets, room * sizeof(*grown));

        if (grown == NULL) {
            return NULL;
        }

        d->sheets = grown;
        d->sheets_room = room;
    }

    memset(&d->sheets[d->nsheets], 0, sizeof(d->sheets[0]));
    return &d->sheets[d->nsheets++];
}

/* One element of the walk: a `<style>` made now, a `<link>` noted. */
static void sheet_from_element(struct web_ns_doc *d, dom_node *node,
                               dom_string *tag)
{
    bool quirks = d->html.quirks != DOM_DOCUMENT_QUIRKS_MODE_NONE;

    if (dom_string_caseless_lwc_isequal(tag, corestring_lwc_style)) {
        dom_string *text = NULL;

        if (for_screen(node)
            && dom_node_get_text_content(node, &text) == DOM_NO_ERR
            && text != NULL) {
            css_stylesheet *sheet = sheet_of(dom_string_data(text),
                    dom_string_byte_length(text),
                    nsurl_access(d->html.base_url), quirks);
            struct page_sheet *at = sheet != NULL ? more_sheet(d) : NULL;

            if (at != NULL) {
                at->sheet = sheet;
            } else if (sheet != NULL) {
                css_stylesheet_destroy(sheet);
            }
        }

        if (text != NULL) {
            dom_string_unref(text);
        }
    } else if (dom_string_caseless_lwc_isequal(tag, corestring_lwc_link)
               && rel_is_stylesheet(node) && for_screen(node)) {
        dom_string *href = NULL;
        nsurl *url = NULL;

        if (dom_element_get_attribute(node, corestring_dom_href, &href)
            == DOM_NO_ERR && href != NULL) {
            if (nsurl_join(d->html.base_url, dom_string_data(href), &url)
                == NSERROR_OK) {
                struct page_sheet *at = more_sheet(d);

                if (at != NULL) {
                    at->url = url;
                } else {
                    nsurl_unref(url);
                }
            }

            dom_string_unref(href);
        }
    }
}

/*
 * Every `<style>` and `<link rel="stylesheet">` of the page, in document
 * order, which is the order the cascade takes them in: a walk of the tree
 * by its first children and next siblings, back up by parents, so it needs
 * no stack however deep the page nests.
 */
static void page_sheets(struct web_ns_doc *d)
{
    dom_node *at = NULL;

    if (dom_document_get_document_element(d->html.document, (void *)&at)
        != DOM_NO_ERR) {
        return;
    }

    while (at != NULL) {
        dom_node *next = NULL;
        dom_node_type type;

        if (dom_node_get_node_type(at, &type) == DOM_NO_ERR
            && type == DOM_ELEMENT_NODE) {
            dom_string *tag = NULL;

            if (dom_node_get_node_name(at, &tag) == DOM_NO_ERR
                && tag != NULL) {
                sheet_from_element(d, at, tag);
                dom_string_unref(tag);
            }

            (void)dom_node_get_first_child(at, &next);
        }

        while (next == NULL && at != NULL) {
            dom_node *parent = NULL;

            (void)dom_node_get_next_sibling(at, &next);

            if (next != NULL) {
                break;
            }

            (void)dom_node_get_parent_node(at, &parent);
            dom_node_unref(at);
            at = parent;

            if (at != NULL) {
                dom_node_type ptype;

                if (dom_node_get_node_type(at, &ptype) == DOM_NO_ERR
                    && ptype == DOM_DOCUMENT_NODE) {
                    dom_node_unref(at);
                    at = NULL;
                }
            }
        }

        if (at != NULL) {
            dom_node_unref(at);
        }

        at = next;
    }
}

/* The links' addresses still to be fetched: `(n, url)` for each. */
size_t web_ns_sheets(struct web_ns_doc *d, size_t k, const char **url)
{
    size_t i, seen = 0;

    for (i = 0; i < d->nsheets; i++) {
        if (d->sheets[i].url != NULL && d->sheets[i].sheet == NULL) {
            if (seen++ == k) {
                *url = nsurl_access(d->sheets[i].url);
                return i + 1;
            }
        }
    }

    return 0;
}

/* A link's text, fetched, made its sheet. */
bool web_ns_sheet(struct web_ns_doc *d, size_t n, const char *text,
                  size_t len)
{
    struct page_sheet *at;

    if (n == 0 || n > d->nsheets || d->built) {
        return false;
    }

    at = &d->sheets[n - 1];

    if (at->url == NULL || at->sheet != NULL) {
        return false;
    }

    at->sheet = sheet_of(text, len, nsurl_access(at->url),
                         d->html.quirks != DOM_DOCUMENT_QUIRKS_MODE_NONE);
    return at->sheet != NULL;
}

struct web_ns_doc *web_ns_open(void *document, const char *base,
                               const char *charset)
{
    struct web_ns_doc *d;
    html_content *h;

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

    /* What the page was read in, and what its forms are sent in. */
    h->encoding = strdup(charset != NULL && charset[0] != '\0' ? charset
                                                                : "UTF-8");
    h->background_colour = NS_TRANSPARENT;
    h->media.type = CSS_MEDIA_SCREEN;
    h->font_func = &layout_table;
    h->bctx = talloc_zero(NULL, int);
    h->unit_len_ctx.device_dpi = zoomed_dpi();
    h->unit_len_ctx.font_size_default = INTTOFIX(16);
    h->unit_len_ctx.font_size_minimum = INTTOFIX(6);

    if (lwc_intern_string("*", 1, &h->universal) != lwc_error_ok
        || h->bctx == NULL || h->base_url == NULL || h->encoding == NULL) {
        web_ns_close(d, NULL);      /* no pictures yet, so none to let go */
        return NULL;
    }

    page_sheets(d);
    return d;
}

/* The cascade, from NetSurf's defaults and the page's sheets in order -
 * made once, at the first layout, when every link has had its chance. */
static bool make_cascade(struct web_ns_doc *d)
{
    html_content *h = &d->html;
    size_t i;

    if (css_select_ctx_create(&h->select_ctx) != CSS_OK) {
        return false;
    }

    (void)css_select_ctx_append_sheet(h->select_ctx, ua_default,
                                      CSS_ORIGIN_UA, NULL);

    if (ua_quirks != NULL && h->quirks != DOM_DOCUMENT_QUIRKS_MODE_NONE) {
        (void)css_select_ctx_append_sheet(h->select_ctx, ua_quirks,
                                          CSS_ORIGIN_UA, NULL);
    }

    for (i = 0; i < d->nsheets; i++) {
        if (d->sheets[i].sheet != NULL) {
            (void)css_select_ctx_append_sheet(h->select_ctx,
                                              d->sheets[i].sheet,
                                              CSS_ORIGIN_AUTHOR, NULL);
        }
    }

    return true;
}

/*
 * The page's forms, before its box tree, which makes a control for each
 * field and asks for the form it belongs to - as NetSurf's `html.c` does,
 * every action made absolute against the page's address, an empty one
 * being the page's own (HTML 4.10.22.3, step 9).
 */
static bool page_forms(struct web_ns_doc *d)
{
    html_content *h = &d->html;
    struct form *f;

    h->bw = THE_WINDOW;
    h->forms = html_forms_get_forms(h->encoding != NULL ? h->encoding
                                                        : "UTF-8",
                                    (dom_html_document *)h->document);

    for (f = h->forms; f != NULL; f = f->prev) {
        nsurl *action = NULL;
        const char *against = f->action != NULL && f->action[0] != '\0'
                              ? f->action : nsurl_access(h->base_url);

        if (nsurl_join(h->base_url, against, &action) != NSERROR_OK) {
            return false;
        }

        free(f->action);
        f->action = strdup(nsurl_access(action));
        nsurl_unref(action);

        if (f->action == NULL) {
            return false;
        }
    }

    return true;
}

/* Laid out at `width`; the page's whole height, or -1. The box tree is
 * built the first time, and a new width lays the same tree out again. */
/*
 * **What a layout measured, forgotten**, for the zoom: NetSurf measures a
 * run of text once and keeps its width and the space after it in its box,
 * and keeps each box's least and most widths, for every layout after - so
 * a page laid out again at 150% placed each run after the one before at
 * the width it had at 100%, and runs in another face ran into each other.
 * Put back to what a box is made with, `UNKNOWN_WIDTH` and
 * `UNKNOWN_MAX_WIDTH`, so the next layout measures in the new size. The
 * tree walked by its links rather than by recursion, which a deep page
 * would take a stack's depth of.
 */
static void unmeasure(struct box *top)
{
    struct box *at = top;

    while (at != NULL) {
        if (at->text != NULL) {
            at->width = UNKNOWN_WIDTH;
        }

        if (at->space != 0) {
            at->space = UNKNOWN_WIDTH;
        }

        at->min_width = 0;
        at->max_width = UNKNOWN_MAX_WIDTH;

        /* A list item's marker hangs off it rather than among its children,
         * and is measured as a run of text is: the bullet that touched its
         * words at 150% was the one box the walk did not reach. */
        if (at->list_marker != NULL) {
            at->list_marker->width = UNKNOWN_WIDTH;
            at->list_marker->space = at->list_marker->space != 0 ? UNKNOWN_WIDTH : 0;
            at->list_marker->max_width = UNKNOWN_MAX_WIDTH;
        }

        if (at->children != NULL) {
            at = at->children;
            continue;
        }

        while (at != NULL && at != top && at->next == NULL) {
            at = at->parent;
        }

        if (at == NULL || at == top) {
            break;
        }

        at = at->next;
    }
}

int web_ns_layout(struct web_ns_doc *d, lua_State *L, int width, int height)
{
    html_content *h = &d->html;
    struct box *top;
    unsigned long began;
    int tall;

    faces_L = L;
    d->why = NULL;

    /* At the zoom in force now, which may have changed since the last - and
     * then what the last measured, at the old size, measured again. */
    h->unit_len_ctx.device_dpi = zoomed_dpi();

    if (d->laid_zoom != 0 && d->laid_zoom != zoom_pct && h->layout != NULL) {
        unmeasure(h->layout);
    }

    d->laid_zoom = zoom_pct;

    if (!d->built) {
        dom_node *root = NULL;
        nserror e;

        d->built = true;

        if (!make_cascade(d)) {
            d->why = "no memory for the cascade";
            faces_L = NULL;
            return -1;
        }

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

        if (!page_forms(d)) {
            d->why = "no memory for the page's forms";
            dom_node_unref(root);
            faces_L = NULL;
            return -1;
        }

        converting = d;
        began = kosmos_ticks();
        e = dom_to_box(root, h, converted, &h->box_conversion_context);

        if (e == NSERROR_OK) {
            web_netsurf_run();
        }

        d->costs.boxes.ticks = kosmos_ticks() - began;
        d->costs.boxes.calls = 1;
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

    began = kosmos_ticks();

    if (!layout_document(h, width, height)) {
        d->why = "NetSurf's layout ran out of memory";
        faces_L = NULL;
        return -1;
    }

    d->costs.layout.ticks = kosmos_ticks() - began;
    d->costs.layout.calls++;

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

/*
 * The band of the page starting `from` rows down, drawn into `s` - all of
 * it, or only where it meets `area` ({ x, y, w, h } on the page), which is
 * what a keystroke in a field asks for. The caret is drawn here, since a
 * form's field leaves it to the browser, as every NetSurf front end has it.
 */
void web_ns_paint(struct web_ns_doc *d, lua_State *L, struct surface *s,
                  int width, int height, long from, const int *area)
{
    struct paint p;
    struct redraw_context ctx;
    struct content_redraw_data data;
    struct rect clip = { 0, 0, width, height };

    if (d->html.layout == NULL) {
        return;
    }

    if (area != NULL) {
        int x0 = area[0], y0 = (int)(area[1] - from);
        int x1 = x0 + area[2], y1 = y0 + area[3];

        if (x0 > clip.x0) clip.x0 = x0;
        if (y0 > clip.y0) clip.y0 = y0;
        if (x1 < clip.x1) clip.x1 = x1;
        if (y1 < clip.y1) clip.y1 = y1;

        if (clip.x0 >= clip.x1 || clip.y0 >= clip.y1) {
            return;
        }
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

    memset(&costs, 0, sizeof(costs));
    costs.whole.calls = 1;
    costs.whole.ticks = kosmos_ticks();
    faces_L = L;
    (void)html_redraw(&d->html.base, &data, &clip, &ctx);
    faces_L = NULL;
    costs.whole.ticks = kosmos_ticks() - costs.whole.ticks;
    /* The layout's own, which a paint does not measure, kept across it. */
    costs.boxes = d->costs.boxes;
    costs.layout = d->costs.layout;
    d->costs = costs;

    if (d->focus != NULL) {
        long cx = d->caret_x, cy = d->caret_y - from, ch = d->caret_h;
        long top = cy < clip.y0 ? clip.y0 : cy;
        long bottom = cy + ch > clip.y1 ? clip.y1 : cy + ch;

        if (cx >= clip.x0 && cx < clip.x1 && top < bottom) {
            gfx_draw_fill(s, cx, top, 1, bottom - top, 0xff000000u);
        }
    }
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

/* The fields outside any form, which nothing else frees: a form frees its
 * own (`form_free`), and these belong to none. NetSurf lets them go with
 * the page's memory; a page here is one of many a session opens. */
static void free_lone_controls(struct box *box)
{
    for (; box != NULL; box = box->next) {
        if (box->gadget != NULL && box->gadget->form == NULL
            && box->gadget->box == box) {
            form_free_control(box->gadget);
            box->gadget = NULL;
        }

        free_lone_controls(box->children);
    }
}

/*
 * `L` is whoever is closing it - the pictures' references are let go
 * through it. It is never one kept from earlier: a load runs in a coroutine
 * now (`roadmap.md` 6zz l3), and the state that handed a picture over can be
 * a coroutine long ended and collected by the time the page is closed -
 * which is what a kept `d->L` was, and what closing the browser's test
 * page from the Dam article's load read through: a data abort in
 * `luaH_getint`, at an address of 0xd.
 */
void web_ns_close(struct web_ns_doc *d, lua_State *L)
{
    html_content *h;
    struct hlcache_handle *o, *next;
    struct form *f, *g;
    size_t i;

    if (d == NULL) {
        return;
    }

    h = &d->html;

    if (h->select_ctx != NULL) {
        css_select_ctx_destroy(h->select_ctx);
    }

    for (i = 0; i < d->nsheets; i++) {
        if (d->sheets[i].sheet != NULL) {
            css_stylesheet_destroy(d->sheets[i].sheet);
        }

        if (d->sheets[i].url != NULL) {
            nsurl_unref(d->sheets[i].url);
        }
    }

    free(d->sheets);

    for (o = d->objects; o != NULL; o = next) {
        next = o->next;

        if (o->ref != LUA_NOREF && L != NULL) {
            luaL_unref(L, LUA_REGISTRYINDEX, o->ref);
        }

        nsurl_unref(o->url);
        free(o);
    }

    /* The forms before the boxes, as NetSurf's `html_destroy` has it. */
    free_lone_controls(h->layout);

    for (f = h->forms; f != NULL; f = g) {
        g = f->prev;
        form_free(f);
    }

    free(d->sent_url);
    free(d->sent_body);
    free(h->encoding);

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

/*--------------------------------------------------------------------------
 * Pictures, as NetSurf's objects (`roadmap.md` 6zz j4).
 *
 * NetSurf's layout asks for a picture - an `<img>`, an `<object>`, a
 * background - through `html_fetch_object`, and gets it later as a content
 * it measures and draws. Here the asking is noted, the browser fetches and
 * decodes as it always has - side by side, the band's first - and hands the
 * decoded surface back (`web_ns_picture`); from then the box has its object,
 * the layout its natural size and `content_redraw` the pixels, scaled to the
 * box as the page is drawn. A picture that never arrives leaves its box laid
 * out by what the page said of it, as NetSurf lays one out still loading.
 *
 * `struct hlcache_handle`, above the document, is the whole of one here.
 *------------------------------------------------------------------------*/

bool html_fetch_object(struct html_content *c, struct nsurl *url,
                       struct box *box, content_type permitted_types,
                       bool background)
{
    struct web_ns_doc *d = (struct web_ns_doc *)c;
    struct hlcache_handle *o;

    (void)permitted_types;

    o = calloc(1, sizeof(*o));

    if (o == NULL) {
        return false;
    }

    o->box = box;
    o->url = nsurl_ref(url);
    o->background = background;
    o->ref = LUA_NOREF;
    o->next = d->objects;
    d->objects = o;
    d->nobjects++;
    return true;
}

/* The `k`th picture asked for, in page order: its address, where its box
 * is on the page and how big, and whether it has arrived. */
bool web_ns_object(struct web_ns_doc *d, size_t k, const char **url,
                   int *x, int *y, int *w, int *h, bool *background,
                   bool *arrived)
{
    struct hlcache_handle *o = d->objects;
    size_t from_end = d->nobjects - 1 - k;

    if (k >= d->nobjects) {
        return false;
    }

    while (from_end-- > 0) {
        o = o->next;
    }

    *url = nsurl_access(o->url);
    box_coords(o->box, x, y);
    *w = o->box->width;
    *h = o->box->height;
    *background = o->background;
    *arrived = o->pic != NULL;
    return true;
}

size_t web_ns_objects(struct web_ns_doc *d)
{
    return d->nobjects;
}

/* The `k`th picture, arrived: the surface on top of `L`'s stack, held in
 * its registry for as long as the document is, and its natural size. */
bool web_ns_picture(struct web_ns_doc *d, lua_State *L, size_t k, int width,
                    int height)
{
    struct hlcache_handle *o = d->objects;
    size_t from_end = d->nobjects - 1 - k;

    if (k >= d->nobjects) {
        lua_pop(L, 1);
        return false;
    }

    while (from_end-- > 0) {
        o = o->next;
    }

    if (o->ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, o->ref);
    }

    o->pic = luaL_checkudata(L, -1, "kosmos.surface");
    o->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    o->width = width;
    o->height = height;

    if (o->background) {
        o->box->background = o;
    } else {
        o->box->object = o;
    }

    return true;
}

/* NetSurf's questions about a content, for a picture that has arrived. */
content_type content_factory_type_from_mime_type(lwc_string *mime_type)
{
    (void)mime_type;
    return CONTENT_IMAGE;
}

struct nsurl *hlcache_handle_get_url(const struct hlcache_handle *handle)
{
    return handle->url;
}

content_type content_get_type(struct hlcache_handle *h)
{
    (void)h;
    return CONTENT_IMAGE;
}

/* A picture's own size, in the screen's pixels at the zoom in force. */
int content_get_width(struct hlcache_handle *h)
{
    return h->width * zoom_pct / 100;
}

int content_get_height(struct hlcache_handle *h)
{
    return h->height * zoom_pct / 100;
}

int content_get_available_width(struct hlcache_handle *h)
{
    return h->width * zoom_pct / 100;
}

bool content_get_opaque(struct hlcache_handle *h)
{
    (void)h;
    return false;
}

bool content_can_reformat(struct hlcache_handle *h)
{
    (void)h;
    return false;
}

void content_reformat(struct hlcache_handle *h, bool background, int width,
                      int height)
{
    (void)h;
    (void)background;
    (void)width;
    (void)height;
}

/*
 * A picture drawn where its box says, at the size it says - once, or tiled
 * across the clip for a background that repeats - within the clip the box
 * gave and the one the plotter holds.
 */
bool content_redraw(struct hlcache_handle *h, struct content_redraw_data *data,
                    const struct rect *clip, const struct redraw_context *ctx)
{
    const struct paint *p = ctx->priv;
    int cx0 = clip->x0 > p->clip.x0 ? clip->x0 : p->clip.x0;
    int cy0 = clip->y0 > p->clip.y0 ? clip->y0 : p->clip.y0;
    int cx1 = clip->x1 < p->clip.x1 ? clip->x1 : p->clip.x1;
    int cy1 = clip->y1 < p->clip.y1 ? clip->y1 : p->clip.y1;
    int x0 = data->x, y0 = data->y, x, y;
    unsigned long began = kosmos_ticks();
    unsigned pw, ph;

    if (h->pic == NULL || data->width <= 0 || data->height <= 0
        || cx0 >= cx1 || cy0 >= cy1) {
        return true;
    }

    if (data->repeat_x) {
        while (x0 > cx0) x0 -= data->width;
        while (x0 + data->width <= cx0) x0 += data->width;
    }

    if (data->repeat_y) {
        while (y0 > cy0) y0 -= data->height;
        while (y0 + data->height <= cy0) y0 += data->height;
    }

    for (y = y0; y < cy1; y += data->height) {
        for (x = x0; x < cx1; x += data->width) {
            gfx_draw_stretch(p->s, h->pic, x, y, data->width, data->height,
                             cx0, cy0, cx1, cy1);

            if (!data->repeat_x) {
                break;
            }
        }

        if (!data->repeat_y) {
            break;
        }
    }

    /* A page's pictures come here and not through `plot_bitmap`, so they
     * are counted here (`costs`, above) - scaled apart from those at their
     * own size, which are a row blended and cost far less. */
    gfx_draw_size(h->pic, &pw, &ph);

    if (data->width == (int)pw && data->height == (int)ph) {
        costs.pictures.ticks += kosmos_ticks() - began;
        costs.pictures.calls++;
    } else {
        costs.scaled.ticks += kosmos_ticks() - began;
        costs.scaled.calls++;
    }

    return true;
}
