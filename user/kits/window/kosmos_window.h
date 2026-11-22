/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_WINDOW_H
#define KOSMOS_WINDOW_H

/*
 * The Window Kit (`docs/windowkit.md`): a window from C, with no Lua of the
 * application's own.
 *
 *   struct kw_window *w = kw_open("Plasma", 640, 400, KW_RESIZABLE);
 *   struct kw_event e;
 *
 *   for (;;) {
 *       struct kw_surface s = kw_surface(w);
 *       draw(s.pixels, s.width, s.height, s.pitch);
 *       kw_commit(w, 0, 0, s.width, s.height);
 *
 *       while (kw_poll(w, &e, 16)) {
 *           if (e.type == KW_CLOSE) { kw_close(w); return 0; }
 *           ...
 *       }
 *   }
 *
 * **The pixels are the window's own**, in the region the window manager
 * composes from: two buffers, and `kw_commit` makes the one just drawn the
 * one shown - nothing is copied. Ask `kw_surface` again after every commit
 * and every poll, since the other buffer is the next one to draw into and a
 * resize replaces both; a surface from before either is not to be used.
 *
 * **The pitch is in bytes and is almost never `width * 4`**: a row starts
 * at `(char *)pixels + y * pitch`. Pixels are 0xAARRGGBB.
 */

#include <stdint.h>

/* kw_open's flags. */
#define KW_RESIZABLE  1u        /* a grip, and KW_RESIZE events */
#define KW_CENTRE     2u        /* in the middle of the screen */

/* What happened: kw_event's type. */
#define KW_KEY      1           /* key: a character, or one of keys.lua's codes */
#define KW_POINTER  2           /* action, button, x, y */
#define KW_WHEEL    3           /* amount, x, y */
#define KW_RESIZE   4           /* width, height: the new surface's size */
#define KW_CLOSE    5           /* the close box, or the window manager gone */
#define KW_RAWKEY   6           /* key: the keycode; action: KW_PRESS or KW_RELEASE */
#define KW_MOTION   7           /* dx, dy: how far the mouse went, with the pointer captured */
#define KW_CAPTURE  8           /* action: KW_PRESS when the pointer is had, KW_RELEASE when lost */

/* A pointer event's action, and its button. */
#define KW_PRESS    1
#define KW_RELEASE  2
#define KW_MOVE     3
#define KW_LEFT     1
#define KW_RIGHT    2

struct kw_event {
    int type;
    int key;                    /* KW_KEY */
    int action, button;         /* KW_POINTER */
    int x, y;                   /* KW_POINTER, KW_WHEEL: in the window */
    int amount;                 /* KW_WHEEL: steps, signed */
    int width, height;          /* KW_RESIZE */
    int dx, dy;                 /* KW_MOTION: right and down, screen pixels */
};

struct kw_surface {
    uint32_t *pixels;           /* NULL when there is nothing to draw into */
    unsigned width, height;
    unsigned pitch;             /* bytes from one row to the next */
};

struct kw_window;

/* A window that draws its own pixels, or NULL - and why, in kw_why(). */
struct kw_window *kw_open(const char *title, unsigned width, unsigned height,
                          unsigned flags);

/* The buffer to draw into now. */
struct kw_surface kw_surface(struct kw_window *w);

/* This frame is drawn; show it. The rectangle is what changed. 0 when the
 * window has gone. */
int kw_commit(struct kw_window *w, unsigned x, unsigned y, unsigned width,
              unsigned height);

/* The next event into `e`: 1, or 0 when none came within `wait_ms`
 * milliseconds. 0 answers at once. */
int kw_poll(struct kw_window *w, struct kw_event *e, unsigned wait_ms);

/* A game controller as the window manager last said it was, for the focused
 * window (`docs/flight.md`, F7): sticks from -1 to 1, up and right
 * positive, triggers from 0 to 1, and `buttons` as `pad_decode.h`'s
 * `enum pad_bit` bits - south (A) is bit 0, east (B) bit 1. */
struct kw_pad {
    int present;                /* 0: no pad in this slot, or not in front */
    uint32_t buttons;
    float lx, ly, rx, ry;
    float lt, rt;
};

/* Pad `n`, from 0, as of the last commit or poll: 1 when one is there. */
int kw_pad(struct kw_window *w, unsigned n, struct kw_pad *out);

/*
 * **The pointer to this window alone, or given back** (`docs/quake3e.md`,
 * Q1): while it is had, the arrow stays still and the mouse's movement comes
 * as KW_MOTION, with no screen edge to stop it - a game looking about.
 * Asked, not taken: the window manager grants it only to the window in
 * front, and gives it back on Super, on a focus change and on close. Either
 * way a KW_CAPTURE event says which, so a game asks again on its next click.
 * 1 when the request was answered.
 */
int kw_capture(struct kw_window *w, int on);

/* Closed and let go. The window is not to be used after. */
void kw_close(struct kw_window *w);

/* Why the last kw_open answered NULL, in words. */
const char *kw_why(void);

#endif /* KOSMOS_WINDOW_H */
