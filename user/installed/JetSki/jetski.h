/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **JetSki's UI process, as a door** (`docs/w7.md` W7c): one page in a view
 * of a size, loaded by address, WebKit's run loop run for a while at a time,
 * and the page's picture painted into pixels the caller owns. The window
 * around it is `jetski.lua`'s; this knows nothing of windows.
 */

#ifndef KOSMOS_JETSKI_H
#define KOSMOS_JETSKI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What happened since the last `jetski_pump`, as flags. */
enum jetski_happened {
    JETSKI_STARTED   = 1 << 0,     /* a navigation began */
    JETSKI_COMMITTED = 1 << 1,     /* its first bytes arrived: a new page */
    JETSKI_FINISHED  = 1 << 2,     /* it finished loading */
    JETSKI_FAILED    = 1 << 3,     /* it failed; `error` says how */
    JETSKI_PICTURE   = 1 << 4,     /* the page has a new picture to paint */
    JETSKI_CRASHED   = 1 << 5,     /* the web process ended */
    JETSKI_ASKED     = 1 << 6,     /* the page asked to go somewhere: a link */
    JETSKI_TABS      = 1 << 7,     /* a tab not shown began, finished or ended */
    JETSKI_INSPECTOR = 1 << 8,     /* the inspector came, went, or has a picture */
};

struct jetski_state {
    unsigned happened;             /* `jetski_happened`, since the last pump */
    int error;                     /* curl's or WebKit's code, with FAILED */
    double progress;               /* 0 to 1, WebKit's estimate */
    int loading;                   /* a navigation is under way */
    int can_back, can_forward;
    char title[256];
    char address[2048];
    char going[2048];              /* where it was asked to go, until it starts */
    int tab;                       /* the tab shown, which all of this is about */
    int opened;                    /* a tab a page opened and showed, or 0 */
    int closing;                   /* a tab whose page asked to close, or 0 */
};

/* A tab, as the strip draws it. */
struct jetski_tab {
    int id;
    int shown, loading;
    int pid;                       /* its web process, the last one seen; 0 before */
    char title[256];
};

/* The view, `width` by `height`, with its first tab; 0 when it was made.
 * Once a process. */
int jetski_open(unsigned width, unsigned height);

/* Tabs: one made (its id, or 0), one shown, one closed - never the last -
 * and the list, in order, as many as `room`. Everything else here is
 * about the tab shown. */
int jetski_tab_new(void);
int jetski_tab_select(int id);
int jetski_tab_close(int id);
unsigned jetski_tabs(struct jetski_tab *out, unsigned room);
void jetski_resize(unsigned width, unsigned height);
/* Where the page's window is on the screen and how big, frame and all:
 * what `window.screenX`, `outerWidth` and the rest answer. */
void jetski_frame(int x, int y, unsigned width, unsigned height);

void jetski_load(const char *address);
void jetski_back(void);
void jetski_forward(void);
void jetski_reload(void);
void jetski_stop(void);

/* WebKit's run loop for at most `ms` milliseconds, or until the page has a
 * new picture; then what happened since the last call. */
void jetski_pump(unsigned ms, struct jetski_state *out);

/*
 * **Why a tab's page stopped** (Diego, 14 November 2026: "why dont we show
 * useful debugging info in the browser when something bad happens?"). Its
 * web process's id while it ran, what it was loading, and - once JetSki has
 * collected it, which may be a pass after WebKit says it ended - what
 * Nebula recorded of its death: `known` 0 until then.
 */
struct jetski_crash {
    int pid;                       /* the web process, as it was spawned */
    int known;                     /* its death has been collected */
    int faulted;                   /* a fault ended it, and the rest says how */
    long code;                     /* its exit code */
    char fault[32];                /* the exception's name */
    unsigned kind, thread;         /* the exception's number; which thread */
    uint64_t pc, addr, sp, detail; /* where, what it touched, its stack, syndrome */
    char address[2048];            /* the page it was showing or loading */
};

/* 1 and `out` filled when tab `id`'s page process ended since it last
 * loaded; 0 when it did not. */
int jetski_crash(int id, struct jetski_crash *out);

/* The page's picture, `width` by `height` from its top left, into the
 * pixels at `pitch` bytes a row. */
void jetski_paint(uint32_t *pixels, unsigned width, unsigned height, unsigned pitch);

/* What a person does to the page, in the view's pixels (W7c). */
enum jetski_mouse { JETSKI_PRESS, JETSKI_RELEASE, JETSKI_MOVE };
enum jetski_button { JETSKI_LEFT, JETSKI_MIDDLE, JETSKI_RIGHT, JETSKI_NONE = -1 };

/* `mods` are `keys.lua`'s bits: 1 Shift, 2 Alt, 4 Control, 8 Super. A press
 * close to the last one in place and time is counted as its second. */
void jetski_mouse(enum jetski_mouse action, enum jetski_button button, int x, int y, unsigned mods);

/* `notches` away from the person, as the window manager counts them. */
void jetski_wheel(int x, int y, int notches, unsigned mods);

/* One key, as `keys.lua` numbers it: a character 0 to 255, or a key that is
 * none - the arrows, Home, the function keys - as a negative number, with
 * its modifiers inside. 0 when it was not one WebKit could be given. */
int jetski_key(int code);

/* Whether the page has the keyboard - not while the address is typed. */
void jetski_focus(int focused);

/*
 * **Web Inspector, docked** (`docs/webkit.html`, *Developer mode*): the
 * shown tab's inspector shown or closed; whether it has one; its picture,
 * as `jetski_paint` paints the page's, at the size `jetski_inspector_size`
 * gave it; and which of the two the pointer, the wheel and keys go to
 * (`jetski_target`, 1 the inspector). Developer features - the inspector,
 * Inspect Element - are on or off for every page (`jetski_developer`),
 * and the front end's files are the folder `jetski_inspector_files` names.
 */
void jetski_developer(int on);
void jetski_inspector_files(const char *folder);
void jetski_inspect(int show);
int  jetski_inspecting(void);
void jetski_inspector_size(unsigned width, unsigned height);
void jetski_paint_inspector(uint32_t *pixels, unsigned width, unsigned height, unsigned pitch);
void jetski_target(int inspector);

#ifdef __cplusplus
}
#endif

#endif
