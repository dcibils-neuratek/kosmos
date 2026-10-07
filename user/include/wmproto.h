/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_WMPROTO_H
#define KOSMOS_WMPROTO_H

/*
 * The window manager's frame path, as a declared shape (`docs/windowkit.md`,
 * step W2).
 *
 * The window manager is Lua and speaks tables (`wmproto.lua`). Two of its
 * requests happen every frame - `commit`, this buffer is drawn, and `poll`,
 * what happened - and over tables each is a table built, serialised and
 * thrown away on both sides, sixty times a second: 1,216 bytes of garbage a
 * frame in a C app that otherwise makes none (`testing.md` 18.433). Here
 * they are fixed structs instead, sent to the window manager's own
 * endpoint - `/Running/wm`, the one whose arrival ends its sleep - and told
 * apart from a table by the message's tag, which is what the tag is for.
 * The window manager reads them with `string.unpack` and answers with
 * `string.pack`, in the layouts below; both sides are held to them by the
 * sizes asserted here and by `run_tcc.py`.
 *
 * Opening, resizing and everything else stay tables: they happen because
 * somebody asked, not because a clock came round.
 *
 * Little-endian, as both of Kosmos's processors are.
 */

#include <stdint.h>

/* The tag a frame request carries: "WMFRAME1". */
#define WM_FRAME_TAG     0x31454d4152464d57ull

#define WM_FRAME_COMMIT  1u     /* x, y, w, h: what changed */
#define WM_FRAME_POLL    2u     /* wait_ticks: how long to hold the answer */

/* The most events one answer carries: `EVENTS_PER_REPLY` in `wm.lua`. */
#define WM_FRAME_EVENTS  12u

struct wm_frame_request {           /* "<I4I4i4i4I4I4I4" */
    uint32_t op;
    uint32_t window;
    int32_t  x, y;
    uint32_t w, h;
    uint32_t wait_ticks;            /* scheduler ticks, never the counter's */
};

/* An event's type. Anything else the window manager says - a move, a
 * menu's - is not sent this way. */
#define WM_EV_KEY        1u     /* a: the code */
#define WM_EV_POINTER    2u     /* action, button; a, b: x, y */
#define WM_EV_WHEEL      3u     /* a, b: x, y; c: steps */
#define WM_EV_RESIZE     4u     /* a, b: the new width and height */
#define WM_EV_CLOSE      5u

#define WM_ACT_PRESS     1u
#define WM_ACT_RELEASE   2u
#define WM_ACT_MOVE      3u

#define WM_BUTTON_LEFT   1u
#define WM_BUTTON_RIGHT  2u

struct wm_frame_event {             /* "<I2I2I2I2i4i4i4" */
    uint16_t type, action, button, unused;
    int32_t  a, b, c;
};

/* 0, or why not. */
#define WM_FRAME_OK          0
#define WM_FRAME_NO_WINDOW   1  /* gone, or never this caller's */
#define WM_FRAME_NO_SURFACE  2  /* a window that does not draw its own pixels */
#define WM_FRAME_BAD         3  /* not a request this shape has */

struct wm_frame_reply {             /* "<i4I4I4" and `count` events */
    int32_t  error;
    uint32_t draw_into;             /* commit: the buffer to draw into next */
    uint32_t count;                 /* poll: how many events follow */
    struct wm_frame_event events[WM_FRAME_EVENTS];
};

_Static_assert(sizeof(struct wm_frame_request) == 28, "wm_frame_request is 28 bytes");
_Static_assert(sizeof(struct wm_frame_event) == 20, "wm_frame_event is 20 bytes");
_Static_assert(sizeof(struct wm_frame_reply) == 12 + 20 * WM_FRAME_EVENTS,
               "wm_frame_reply is its head and its events");

#endif /* KOSMOS_WMPROTO_H */
