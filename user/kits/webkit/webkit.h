/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The WebKit Kit, as a door** (`docs/mailweb.md` W2): Kosmos's one way to
 * WebKit, in no image but `webkit.elf` (`/Kosmos/WebKit`), reached as
 * `use("/Kosmos/Kits/webkit")` by a program that runs in it - JetSki, and
 * Mail's message pane next (W5).
 *
 * **Views**: a page of a size, with options fixed when it is made, painted
 * into pixels the caller owns. JetSki's tabs are views, and the tab strip
 * is JetSki's; a view knows nothing of windows or tabs. WebKit's run loop
 * is run for a while at a time (`webkit_pump`) between the window's own
 * events. This was `jetski.h`, one page with tabs inside it, until W2.
 */

#ifndef KOSMOS_WEBKIT_H
#define KOSMOS_WEBKIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What happened to a view since it was last asked (`webkit_state`). */
enum webkit_happened {
    WEBKIT_STARTED   = 1 << 0,     /* a navigation began */
    WEBKIT_COMMITTED = 1 << 1,     /* its first bytes arrived: a new page */
    WEBKIT_FINISHED  = 1 << 2,     /* it finished loading */
    WEBKIT_FAILED    = 1 << 3,     /* it failed; `error` says how */
    WEBKIT_PICTURE   = 1 << 4,     /* the page has a new picture to paint */
    WEBKIT_CRASHED   = 1 << 5,     /* the web process ended */
    WEBKIT_ASKED     = 1 << 6,     /* the page asked to go somewhere: a link */
    WEBKIT_INSPECTOR = 1 << 7,     /* its inspector came, went, or has a picture */
    WEBKIT_HOVER     = 1 << 8,     /* what is under the pointer changed */
};

/*
 * **A view's options**, fixed when it is made (`docs/mailweb.md`, *The
 * WebKit Kit*): whether its pages run scripts - off for a message, which is
 * a document and never a program; and whether a link it is asked to follow
 * is followed, or only reported (`WEBKIT_ASKED` and `going`) and left to the
 * caller - Mail hands it to JetSki, and its pane stays on the message.
 */
struct webkit_options {
    int scripts;
    int follow;
};

struct webkit_state {
    unsigned happened;             /* `webkit_happened`, since the last ask */
    int error;                     /* curl's or WebKit's code, with FAILED */
    double progress;               /* 0 to 1, WebKit's estimate */
    int loading;                   /* a navigation is under way */
    int can_back, can_forward;
    char title[256];
    char address[2048];
    char going[2048];              /* where it was asked to go, until it starts */
    unsigned blocked;              /* requests a rule list kept out, this page */
    unsigned cookies_blocked;      /* loads whose cookies a rule list kept back */
    int unblocked;                 /* its page was loaded with blocking off */
    int refused;                   /* its page was refused for its certificate */
    int certificate_error;         /* OpenSSL's number for why, with `refused` */
    char refused_reason[256];      /* and OpenSSL's words */
    char refused_address[2048];    /* the address that was refused */
};

/*
 * **How a view's page came** (`docs/padlock.html`): over HTTPS with its
 * certificate checked and nothing plain in it, over plain HTTP, over HTTPS
 * with something plain in it, over HTTPS opened anyway (`webkit_anyway`),
 * or from this machine's disk; NONE for a page of no scheme of those -
 * about:blank. The certificate is what WebKit's network process saw, as
 * far as it handed it on: `chain` 0 when it handed nothing.
 */
enum webkit_came {
    WEBKIT_CAME_NONE,
    WEBKIT_CAME_SECURE,
    WEBKIT_CAME_PLAIN,
    WEBKIT_CAME_MIXED,
    WEBKIT_CAME_ANYWAY,
    WEBKIT_CAME_FILE,
};

struct webkit_security {
    int came;                      /* `webkit_came` */
    int error;                     /* the certificate's, as checked; 0 good */
    unsigned chain;                /* certificates the server sent */
    char subject[256];             /* to whom */
    char issuer[256];              /* by whom */
    char names[512];               /* the names and addresses it is good for */
    double valid_from, valid_until;/* seconds since 1970 */
};

/* How view `id`'s page came; 0 when there is no such view. */
int webkit_security(int id, struct webkit_security *out);

/*
 * **Open anyway** (Diego, 16 November 2026: "Yes, open anyway"): view
 * `id`'s refused page loaded again with its host's certificate taken as it
 * is, for this view alone and remembered nowhere - until the view's page is
 * of another host, when it is forgotten. 0 when nothing was refused.
 */
int webkit_anyway(int id);

/*
 * **What the rule lists kept out of a view's page, by host** (`adblock.md`
 * B4): as many as `room`, the most first; how many hosts there are in all.
 */
struct webkit_kept {
    char host[128];
    unsigned count;
};
unsigned webkit_kept(int id, struct webkit_kept *out, unsigned room);

/*
 * **Sites allowed past blocking**: one host a line, each allowing itself and
 * every host under it. Their pages load from now on with the rule lists off
 * - WebKit's website policies, handed with each navigation's decision.
 */
void webkit_allow_sites(const char *list);

/* What happened to the views together since the last pump. */
struct webkit_events {
    int opened, opener;            /* a view a page opened, and whose page */
    int closing;                   /* a view whose page asked to close, or 0 */
    int others;                    /* a view not shown began, finished or ended */
    int downloads;                 /* a download began, finished or failed */
    int downloading;               /* how many are running */
};

/*
 * **Downloads** (`docs/webkit.html`, *Downloads*): what a page could not
 * show, or its server called an attachment, written by WebKit's network
 * process into the folder named here - `/Home/Downloads` unless JetSki
 * says otherwise - under the name the server suggested, made unique.
 */
enum webkit_download_state {
    WEBKIT_DOWNLOADING,
    WEBKIT_DOWNLOADED,
    WEBKIT_DOWNLOAD_FAILED,
    WEBKIT_DOWNLOAD_CANCELLED,
};

struct webkit_download {
    int id;
    int view;                      /* the view whose page it came from */
    int state;                     /* `webkit_download_state` */
    long long received;            /* bytes so far */
    long long expected;            /* bytes in all; -1 when the server did not say */
    double seconds;                /* how long it ran, or has run */
    int error;                     /* with FAILED, curl's or WebKit's code */
    char name[256];                /* the file's name, in the folder */
    char path[1024];               /* the whole of it; "" until it is decided */
    char address[1024];            /* where it came from */
    char reason[256];              /* with FAILED, WebKit's words */
};

/*
 * **What is under the pointer** (`docs/webkit.html`, *The right button*):
 * a link, its words, a picture, whether it is a field - as WebKit last said,
 * `WEBKIT_HOVER` when it changes - and the words selected, as the page said
 * after the button's last release. 0 when there is no such view.
 */
struct webkit_under {
    char link[2048];
    char label[256];
    char image[2048];
    int editable;
    char selected[1024];
};
int webkit_under(int id, struct webkit_under *out);

void webkit_download_folder(const char *folder);
/* Every download since JetSki began, the newest first; how many. */
unsigned webkit_downloads(struct webkit_download *out, unsigned room);
/* A running one stopped, and its part-written file removed; 0 if none. */
int webkit_download_cancel(int id);
/* One that ended taken off the list - its file stays. */
void webkit_download_forget(int id);

/* A view as a list of them shows it: JetSki's tab strip. */
struct webkit_view_info {
    int id;
    int shown, loading;
    int pid;                       /* its web process, the last one seen; 0 before */
    char title[256];
    char address[1024];            /* its page's, or the stopped page's; "" for none */
};

/* A view, `width` by `height`; its id, or 0. Not shown until `webkit_show`.
 * `options` may be null: scripts on, links followed. */
int webkit_view(unsigned width, unsigned height, const struct webkit_options *options);
/* Its page closed - which ends its web process, unless another shares it -
 * and its id no more. */
void webkit_close(int id);
/* Shown in the window, or dark - unseen and inactive, as a browser's
 * background tabs are. A dark view's news does not end a pump's slice. */
void webkit_show(int id, int shown);
/* Every view of this process, in the order they were made, as many as
 * `room`. */
unsigned webkit_views(struct webkit_view_info *out, unsigned room);

void webkit_resize(int id, unsigned width, unsigned height);
/* Where the window views are in is on the screen and how big, frame and
 * all: what `window.screenX`, `outerWidth` and the rest answer. */
void webkit_frame(int x, int y, unsigned width, unsigned height);

void webkit_load(int id, const char *address);
void webkit_back(int id);
void webkit_forward(int id);
void webkit_reload(int id);
void webkit_stop(int id);

/* Every view's page loads traced in the log, or not (`webkit.cpp`, *A
 * page's load, traced*): each request as it ends, and what a page still
 * waits on every five seconds while it loads. */
void webkit_trace(int on);

/* Woken when WebKit has news: `door` is the window manager's endpoint and
 * `code` the window's, from `watch_code`; a pulse with them answers the
 * window's poll. A door below nought stops it. */
void webkit_wake(long door, unsigned code);

/* For a suite's control only (`run_jetski.py`, `forgetting`): WebKit's
 * buffer not being drawn owes nothing, and shows what it held before. */
void webkit_store_forgets(int on);

/* WebKit's run loop for at most `ms` milliseconds, or until a view shown
 * has news; then what happened to the views together. */
void webkit_pump(unsigned ms, struct webkit_events *out);
/* What happened to view `id` since it was last asked, and how it stands;
 * 0 when there is no such view. */
int webkit_state(int id, struct webkit_state *out);

/*
 * **Why a view's page stopped** (Diego, 14 November 2026: "why dont we show
 * useful debugging info in the browser when something bad happens?"): its
 * web process's id while it ran, what it was loading, and - once the kit
 * has collected it, which may be a pass after WebKit says it ended - what
 * Nebula recorded of its death: `known` 0 until then.
 */
struct webkit_crash {
    int pid;                       /* the web process, as it was spawned */
    int known;                     /* its death has been collected */
    int faulted;                   /* a fault ended it, and the rest says how */
    long code;                     /* its exit code */
    char fault[32];                /* the exception's name */
    unsigned kind, thread;         /* the exception's number; which thread */
    uint64_t pc, addr, sp, detail; /* where, what it touched, its stack, syndrome */
    char address[2048];            /* the page it was showing or loading */
};

/* 1 and `out` filled when view `id`'s page process ended since it last
 * loaded; 0 when it did not. */
int webkit_crash(int id, struct webkit_crash *out);

/* View `id`'s picture, `width` by `height` from its top left, into the
 * pixels at `pitch` bytes a row - only the part `px`, `py`, `pw`, `ph` of
 * it, in the view's pixels, and the rest of `pixels` left as it was. */
void webkit_paint(int id, uint32_t *pixels, unsigned width, unsigned height, unsigned pitch,
                  int px, int py, int pw, int ph);

/* 1 and the rectangle WebKit has said changed in view `id`'s picture
 * since this was last asked, every rectangle it said joined into one; 0
 * when nothing changed. Asking forgets it. */
int webkit_damage(int id, int *x, int *y, int *w, int *h);

/*
 * **The page shown from WebKit's own two buffers** (`webkit.md` *Fast*
 * 1c): 1 and `out` filled when view `id`'s page changed since this was
 * last asked - the buffer, 1 or 2, just finished and to be shown from now,
 * and what changed in it - and WebKit then writes the other; 0 when
 * nothing changed, or when its store is not in a region (then
 * `webkit_paint`). `everything` says all of it changed, for a window
 * manager not shown this store before. `region` is the process's own capability to both
 * buffers, the second `bytes` after the first, rows at `pitch`; a new
 * `generation` is a new store and a new region.
 */
struct webkit_shown {
    long region;
    unsigned width, height, pitch;
    unsigned long bytes;
    int buffer;
    int x, y, w, h;
    unsigned generation;
};

int webkit_shown(int id, int everything, struct webkit_shown *out);

/* What a person does to a view, in its pixels. */
enum webkit_mouse { WEBKIT_PRESS, WEBKIT_RELEASE, WEBKIT_MOVE };
enum webkit_button { WEBKIT_LEFT, WEBKIT_MIDDLE, WEBKIT_RIGHT, WEBKIT_NONE = -1 };

/* `mods` are `keys.lua`'s bits: 1 Shift, 2 Alt, 4 Control, 8 Super. A press
 * close to the last one in place and time is counted as its second. */
void webkit_mouse(int id, enum webkit_mouse action, enum webkit_button button, int x, int y, unsigned mods);

/* `notches` away from the person, as the window manager counts them. */
void webkit_wheel(int id, int x, int y, int notches, unsigned mods);

/* One key, as `keys.lua` numbers it: a character 0 to 255, or a key that is
 * none - the arrows, Home, the function keys - as a negative number, with
 * its modifiers inside. 0 when it was not one WebKit could be given. */
int webkit_key(int id, int code);

/* Whether view `id` has the keyboard. */
void webkit_focus(int id, int focused);

/*
 * **Web Inspector, docked** (`docs/webkit.html`, *Developer mode*): a view's
 * inspector shown or closed; whether it has one; its picture, as
 * `webkit_paint` paints the page's, at the size `webkit_inspector_size`
 * gave every inspector; and which of the two the pointer, the wheel and
 * keys go to (`webkit_target`, 1 the inspector). Developer features are on
 * or off for every view that runs scripts (`webkit_developer`), and the
 * front end's files are the folder `webkit_inspector_files` names.
 */
void webkit_developer(int on);

/*
 * **JetSki's Settings, for every view** (`roadmap.md`, S1): scripts and
 * pictures on or off (`webkit_settings`), and the zoom in per cent - 100,
 * 125, 150, 200 - for every page now and after (`webkit_zoom`).
 */
void webkit_settings(int scripts, int images);
void webkit_zoom(int percent);
void webkit_inspector_files(const char *folder);
void webkit_inspect(int id, int show);
int  webkit_inspecting(int id);
void webkit_inspector_size(unsigned width, unsigned height);
void webkit_paint_inspector(int id, uint32_t *pixels, unsigned width, unsigned height, unsigned pitch);
void webkit_target(int id, int inspector);

#ifdef __cplusplus
}
#endif

#endif
