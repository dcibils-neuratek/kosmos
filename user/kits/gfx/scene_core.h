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
#include "lens.h"

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
    SC_STALE    = 1u << 10,     /* its picture is from before a resize */
    SC_PLATINUM = 1u << 11,     /* Platinum's boxes (`docs/pewter.md` P2):
                                   close alone at `bx`, zoom and collapse
                                   at `bx2` */
    SC_GLASS    = 1u << 12,     /* glass where it draws: over the
                                   wallpaper's glass rather than what is
                                   under it (Liquid Glass LG4). With
                                   SC_BLEND, a strip's; alone, a window's,
                                   frosted only when shown as it is */
    SC_LIVE     = 1u << 13,     /* with SC_GLASS, glass over what is really
                                   under it when the scene has live glass
                                   (LG6): the floating surfaces - the
                                   dock, a strip, a tip, a banner, a menu -
                                   and never the desktop, whose reach is
                                   the screen */
};

/* The lights, from the left: maximise, minimise, close (`OUT.SLOT`) - and
 * under Platinum zoom, collapse and close, in the same slots. */
#define SC_SLOTS 3

struct sc_window {
    long x, y, w, h;
    unsigned flags;
    struct surface *picture;
    long src_w, src_h;
    struct surface *bar;        /* a menu bar above the picture, or NULL */
    long bar_h;
    long bx, by;
    long bx2;                   /* the second run, under `SC_PLATINUM` */
    int held;                   /* the light pressed, its slot, or -1 */

    /*
     * **An inset** (`webkit.md` *Fast* 1c): a second picture of the
     * application's own, laid over the first at `ix`, `iy` in the
     * picture's pixels and `iw` by `ih` - opaque, and scaled as the picture
     * is. A browser's page, which its engine draws into a region of its own
     * so that nothing copies it into the window. NULL for none.
     */
    struct surface *inset;
    long ix, iy, iw, ih;

    /*
     * **A plate** (`docs/astra.md`, *Identity*, S1): Starlight's - the
     * window in front's title on an amber trapezoid at its header's left -
     * a picture the window manager draws, blended over the window at
     * `px`, `py` from its corner in screen pixels, under the three and
     * inside its rounded corners. NULL for none.
     */
    struct surface *plate;
    long px, py;

    /* Described for the window server (`scene.c`), the pictures by slot -
     * `WS_NONE` for none - in place of the surfaces above. */
    int picture_slot, bar_slot, inset_slot, plate_slot;

    /*
     * **Its glass's shape** (Liquid Glass LG4): for a blended glass strip -
     * the dock, a banner - the rounded rectangle its glass is, at `rx`, `ry`
     * from its corner and `rw` by `rh`, rounded at `rr`, in screen pixels.
     * Inside it the glass is bent and lit by a lens made for `rr`, as a
     * window's rim is by the scene's corner. `rr` 0 for none, and the strip
     * is flat glass as before.
     */
    long rx, ry, rw, rh, rr;
};

struct sc_menu {
    long x, y, w, h;
    struct surface *picture;
    int picture_slot;
    unsigned flags;             /* SC_GLASS, or 0: copied as it always was */
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
    /* Starlight's cut on a menu's top-left corner, or 0 (`docs/astra.md` S2). */
    long cut;

    /* The desktop. */
    uint32_t desktop;
    struct surface *wallpaper, *stamp;
    long wx, wy, ww, wh, sx, sy;
    /* The wallpaper blurred (`OUT.glass_wallpaper`), lying where it lies,
     * for an `SC_GLASS` window to be frosted with; NULL for none. */
    struct surface *glass;

    /* **The rim** (Liquid Glass LG3, LG4): how wide a glass window's and
     * menu's bevel is, in pixels, and how much it bends the glass and is
     * lit, in percent - the look's; a bevel of 0 for none. The lens is
     * made from them and `corner` by `scene_compose` when any of the four
     * changes, never in a frame otherwise; `lens_ok` says it was. */
    long rim_bevel, rim_refraction, rim_specular, rim_split;
    uint32_t rim_front;         /* the window in front's light; 0, white */
    uint32_t rim_horizon;       /* a glass surface's top row, 0 none (Starlight S3) */
    struct gfx_lens lens;
    void *lens_mem;
    long lens_of[4];
    bool lens_ok;

    /* Lenses for other radii than the corner - a strip's shape, `rr` - made
     * when a radius is first asked for under a look and kept, `SC_RIMS` of
     * them, the oldest given up for a new one. */
    struct {
        long of[4];             /* radius, bevel, refraction, specular */
        void *mem;
        struct gfx_lens lens;
        bool ok;
    } rims[4];
    unsigned rims_next;

    /*
     * **Live glass** (Liquid Glass LG6; G2 of `docs/astra.md` 4.3): the
     * floating surfaces - a glass strip (the dock, a banner) and a glass
     * menu - over what is really under them rather than the wallpaper's
     * glass. `live_blur` is the radius of each of three box passes, 0 for
     * clear glass, and -1 for none (wallpaper glass). A surface is live
     * only when the rectangle being composed holds all of it and its reach
     * (`scene_widen`); otherwise it is wallpaper glass, as before.
     * `live_mem` is the scratch - what is under a surface and its reach,
     * copied from the back buffer and blurred there - `live_cap` pixels
     * long, grown and never shrunk; `live_made` counts the surfaces made
     * live since it was last set to nought, for `frames` and the tests.
     */
    long live_blur;
    uint32_t *live_mem;
    size_t live_cap;
    struct surface live_glass;
    long live_made;
    bool glass_clamp;           /* past the glass's edge: its edge pixel */
    struct sc_rect cur;         /* the rectangle `scene_compose` is in */

    /* Over everything. */
    bool outline_on;
    long ox, oy, ow, oh;
    uint32_t outline_colour;
    struct surface *osd, *cursor;
    long osd_x, osd_y, cursor_x, cursor_y;
    uint32_t osd_alpha;

    /* A drag's label beside the pointer, last of all. */
    struct surface *badge;
    long badge_x, badge_y;

    /* A panel over every window and under the pointer - the app switcher
     * (`docs/switcher.html`) - blended by its own alpha. */
    struct surface *panel;
    long panel_x, panel_y;

    /* The pictures above, by slot, when described for the window server. */
    int wallpaper_slot, stamp_slot, osd_slot, cursor_slot, badge_slot, panel_slot;
    int glass_slot;

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

/*
 * **The damage widened for live glass** (LG6): each of the `n` rectangles
 * that touches a live surface's reach grows to hold all of it, so the
 * surface is composed whole and over what is under it. How many grew; the
 * caller merges and asks again until none does.
 */
int scene_widen(const struct scene *sc, struct sc_rect *r, int n);

/* Where a window's inset lands on the screen (`sc_window.inset`): `top` and
 * `tall` the picture's part of the frame, below a menu bar. */
void scene_inset_place(const struct sc_window *w, long top, long tall,
                       long *dx, long *dy, long *dw, long *dh);

#endif
