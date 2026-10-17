/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_WINDOWPROTO_H
#define KOSMOS_WINDOWPROTO_H

#include <stdint.h>

/*
 * **The manager door**: what the window manager may ask the window server
 * (`docs/astra-display.md` D3b-2, `user/servers/windowserver.c`).
 *
 * The window server is started by init at boot and holds the screen
 * (decided by Diego, 10 October); it composes what the window manager
 * describes and decides nothing a person would choose. This door is lent
 * only to the system's window manager - `/Kosmos/Programs/wm.lua`, which
 * declares `needs windowserver` - so no other program can take the screen.
 *
 * It grows step by step, each step a version of this header: the greeting
 * first; then the back buffer, the pictures and the scene, each a region
 * handed over once, and composing.
 */

#define WS_OP_HELLO        1u   /* + the window manager's own endpoint:
                                   the screen's size; the door works */
#define WS_OP_BACK         2u   /* + a region: the back buffer, `width` by
                                   `height` at `pitch`, composed into */
#define WS_OP_SCENE        3u   /* + a region: where the scene is written */
#define WS_OP_PICTURE      4u   /* + a region: picture `slot`, `width` by
                                   `height` at `pitch`, from `offset` */
#define WS_OP_FORGET       5u   /* picture `slot` let go */
#define WS_OP_COMPOSE      6u   /* the scene as written, composed; with
                                   WS_SHOW, put on the screen too */

#define WS_SHOW            1u   /* compose's flags */

#define WS_OK              0u
#define WS_ERR_BAD_OP      1u
#define WS_ERR_NO_SCREEN   2u   /* the server was not given the screen */
#define WS_ERR_NO_REGION   3u   /* no region came, or it would not map */
#define WS_ERR_TOO_SMALL   4u   /* the region does not hold what was said */
#define WS_ERR_NO_ROOM     5u   /* no memory for another picture */
#define WS_ERR_NO_SCENE    6u   /* compose before a back buffer and a scene */
#define WS_ERR_BAD_SCENE   7u   /* the scene says more than its region holds */

/*
 * **Slots are the window manager's numbers for its pictures**, chosen by
 * it, below WS_SLOTS_MAX; a slot sent again is replaced. A picture is
 * named in the scene by its slot, or WS_NONE.
 */
#define WS_SLOTS_MAX       65536u
#define WS_NONE            (-1)

struct ws_request {
    uint32_t op;
    uint32_t slot;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t flags;
    uint64_t offset;
};

struct ws_reply {
    uint32_t error;
    uint32_t width;             /* hello: the screen, in pixels;
                                   compose: the pictures held */
    uint32_t height;
    uint32_t drawn;             /* compose: the pixels drawn */
    /* compose: how long composing took and how long the copy to the
     * screen, apart (`m700-2d.md` step 4a) - microseconds, said here
     * rather than counter ticks, which mean nothing in another process. */
    uint32_t compose_us;
    uint32_t copy_us;
};

_Static_assert(sizeof(struct ws_request) == 32,
               "the window server's request: wm.lua packs \"<I4I4I4I4I4I4I8\"");
_Static_assert(sizeof(struct ws_reply) == 24,
               "the window server's reply is six words: wm.lua reads \"<I4I4I4I4I4I4\"");

/*
 * **The scene, as written into the region they share** (`WS_OP_SCENE`):
 * it changes every pass, so it is data rather than a message - the head,
 * then `nwin` windows from the back, `nmenu` menus, and `nrect` damaged
 * rectangles, one after another. What each field means is
 * `user/kits/gfx/scene_core.h`'s, whose `struct scene` this is the wire
 * shape of: written by the Graphics Kit's scene (`scene.c`) and read by
 * the window server, which holds every count to what the region holds.
 */
#define WS_SCENE_MAGIC     0x6e656373u      /* "scen" */

struct ws_scene_head {
    uint32_t magic;
    uint32_t nwin, nmenu, nrect;

    int32_t  corner, shadow, grip;
    uint32_t edge_light, edge_dark;
    int32_t  box, box_w, run, d;
    uint32_t track, line_soft;
    int32_t  outline;
    uint32_t fill[3], ring[3], mark[3], held_fill[3];

    uint32_t desktop;
    int32_t  wallpaper, wx, wy, ww, wh;
    int32_t  stamp, sx, sy;

    uint32_t outline_on;
    int32_t  ox, oy, ow, oh;
    uint32_t outline_colour;
    int32_t  osd, osd_x, osd_y;
    uint32_t osd_alpha;
    int32_t  cursor, cursor_x, cursor_y;
    int32_t  badge, badge_x, badge_y;
    int32_t  panel, panel_x, panel_y;
};

struct ws_scene_window {
    int32_t  x, y, w, h;
    uint32_t flags;
    int32_t  picture, src_w, src_h;
    int32_t  bar, bar_h;
    int32_t  bx, by, held;
    int32_t  reserved;
};

struct ws_scene_menu {
    int32_t  x, y, w, h;
    int32_t  picture;
    int32_t  reserved;
};

struct ws_rect {
    int32_t  x, y, w, h;
};

_Static_assert(sizeof(struct ws_scene_window) == 56, "a window on the wire is 14 words");
_Static_assert(sizeof(struct ws_scene_menu) == 24, "a menu on the wire is six words");

#endif /* KOSMOS_WINDOWPROTO_H */
