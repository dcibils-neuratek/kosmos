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
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"

#include "utils/corestrings.h"
#include "utils/errors.h"
#include "utils/idna.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/nsurl.h"
#include "utils/string.h"
#include "netsurf/browser_window.h"
#include "netsurf/content.h"
#include "netsurf/content_type.h"
#include "netsurf/layout.h"
#include "netsurf/misc.h"
#include "netsurf/plot_style.h"
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

#include "kits/gfx/gfx_draw.h"
#include "web_netsurf.h"

/*--------------------------------------------------------------------------
 * Once, before anything of NetSurf's runs: its interned names, which its
 * URLs, its styles and its boxes all compare against.
 *------------------------------------------------------------------------*/

static bool ready;

static bool netsurf_ready(void)
{
    if (!ready && corestrings_init() == NSERROR_OK) {
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
 * The platform's: measuring text.
 *
 * On `gfx`, in the face the page's text is drawn in. j1 measures everything
 * in the default face; j2 chooses a face by the style's family, weight,
 * slant and size.
 *------------------------------------------------------------------------*/

static int face_for(const plot_font_style_t *fstyle)
{
    (void)fstyle;
    return 0;
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

/* NetSurf's log. Quiet: the layout's diagnostics are for its authors. */
void nslog_log(const char *file, const char *func, int ln,
               const char *format, ...)
{
    (void)file;
    (void)func;
    (void)ln;
    (void)format;
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

const char *messages_get_errorcode(nserror code)
{
    (void)code;
    return "error";
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
 * Forms: drawn as the boxes their elements make, with no controls in them.
 * NetSurf's `form.c` is a thousand lines of editing and submitting, which a
 * browser that sends nothing anywhere does not need yet.
 */
struct form_control *html_forms_get_control_for_node(struct form *forms,
                                                     dom_node *node)
{
    (void)forms;
    (void)node;
    return NULL;
}

bool form_add_option(struct form_control *control, char *value, char *text,
                     bool selected, void *node)
{
    (void)control;
    (void)value;
    (void)text;
    (void)selected;
    (void)node;
    return true;
}

void form_free_control(struct form_control *control)
{
    (void)control;
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
