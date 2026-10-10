/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_GFX_SCENE_CORE_H
#define KOSMOS_GFX_SCENE_CORE_H

/*
 * **The scene, with no Lua** (Astra's D3b-1, `docs/astra-display.md`): what
 * the desktop is made of and how one damaged rectangle of it is composed -
 * what `gfx.scene` binds for the window manager now, and what the window
 * server will hold. `scene.c` is the binding and says what each part means.
 */

#include <stdbool.h>
#include <stdint.h>

#include "surface.h"

/* What a window is, as bits of `flags`. */
enum {
    SC_HIDDEN   = 1u << 0,
    SC_BLEND    = 1u << 1,      /* the backdrop, or a blended strip */
    SC_ROUNDED  = 1u << 2,      /* an ordinary window: corners and a place
                                   in the occlusion test for them */
    SC_SHADOW   = 1u << 3,
    SC_DIM      = 1u << 4,      /* its shadow lighter: headed, not focused */
    SC_GRIP     = 1u << 5,
    SC_LIGHTS   = 1u << 6,      /* the three, at `bx`, `by` */
    SC_LIT      = 1u << 7,      /* the pointer over them */
    SC_FOCUSED  = 1u << 8,
    SC_MAXIMISE = 1u << 9,      /* it can be maximised */
};

/* The lights, from the left: maximise, minimise, close (`OUT.SLOT`). */
#define SC_SLOTS 3

struct sc_window {
    long x, y, w, h;
    unsigned flags;
    struct surface *picture;
    long src_w, src_h;
    struct surface *bar;        /* a menu bar above the picture, or NULL */
    long bar_h;
    long bx, by;
    int held;                   /* the light pressed, its slot, or -1 */
};

struct sc_menu {
    long x, y, w, h;
    struct surface *picture;
};

struct sc_rect {
    long x, y, w, h;
};

struct scene {
    struct surface *back;

    struct sc_window *win;
    int nwin, capwin;
    struct sc_menu *menu;
    int nmenu, capmenu;

    /* The look. */
    long corner, shadow, grip;
    uint32_t edge_light, edge_dark;
    long box, box_w, run, d;
    uint32_t track, line_soft;
    uint32_t fill[SC_SLOTS], ring[SC_SLOTS], mark[SC_SLOTS], held_fill[SC_SLOTS];
    long outline;

    /* The desktop. */
    uint32_t desktop;
    struct surface *wallpaper, *stamp;
    long wx, wy, ww, wh, sx, sy;

    /* Over everything. */
    bool outline_on;
    long ox, oy, ow, oh;
    uint32_t outline_colour;
    struct surface *osd, *cursor;
    long osd_x, osd_y, cursor_x, cursor_y;
    uint32_t osd_alpha;

    /* Scratch: the pieces of a rectangle not yet covered, two lists that
     * alternate, each `cap_pieces` long, and what each window shows of it -
     * nothing when its width is 0. */
    struct sc_rect *pieces[2];
    int cap_pieces;
    struct sc_rect *visible;
    int cap_visible;

    /* The corners kept from under a window (`OUT.keep_surface`). */
    uint32_t keep_pixels[64 * 64];
    struct surface keep;
};

/* A scene drawing into `back`, empty. */
void scene_init(struct scene *sc, struct surface *back);

/* What it holds given back; the scene is empty after. */
void scene_free(struct scene *sc);

/* Room for `n` windows, or `n` menus, which are then the scene's; false
 * when there is no memory, and nothing changed. */
bool scene_set_windows(struct scene *sc, int n);
bool scene_set_menus(struct scene *sc, int n);

/* One damaged rectangle into the back buffer: the pixels drawn, or -1 when
 * there was no memory for its scratch. */
long scene_compose(struct scene *sc, long x, long y, long w, long h);

#endif
