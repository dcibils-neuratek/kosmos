/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The scene composed - one damaged rectangle of the desktop into the back
 * buffer - with no Lua (Astra's D3b-1). Moved out of `scene.c`, which binds
 * it for the window manager and says what each part of it means; the
 * window server will hold the same. Drawn with the kit's primitives
 * (`draw.c`).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "surface.h"
#include "draw_primitives.h"
#include "shadow.h"
#include "scene_core.h"

void scene_init(struct scene *sc, struct surface *back)
{
    memset(sc, 0, sizeof(*sc));
    sc->back = back;
    sc->keep.pixels = sc->keep_pixels;
    sc->keep.width = sc->keep.height = 64;
    sc->keep.pitch = 64 * 4;
    sc->outline = 2;
}

void scene_free(struct scene *sc)
{
    free(sc->win);
    free(sc->menu);
    free(sc->pieces[0]);
    free(sc->pieces[1]);
    free(sc->visible);
    sc->win = NULL;
    sc->menu = NULL;
    sc->pieces[0] = sc->pieces[1] = NULL;
    sc->visible = NULL;
    sc->nwin = sc->nmenu = sc->capwin = sc->capmenu = 0;
    sc->cap_pieces = sc->cap_visible = 0;
}

/* `list` at least `want` of `each` long, doubling; NULL when there is no
 * memory, and the list as it was. */
static void *grow(void *list, int *cap, int want, size_t each)
{
    void *bigger;
    int n;

    if (want <= *cap) {
        return list;
    }

    n = *cap > 0 ? *cap : 8;

    while (n < want) {
        n *= 2;
    }

    bigger = realloc(list, (size_t)n * each);

    if (bigger == NULL) {
        return NULL;
    }

    *cap = n;
    return bigger;
}

bool scene_set_windows(struct scene *sc, int n)
{
    struct sc_window *w = grow(sc->win, &sc->capwin, n, sizeof(*sc->win));
    struct sc_rect *v;

    if (w == NULL && n > 0) {
        return false;
    }

    sc->win = w;
    v = grow(sc->visible, &sc->cap_visible, n, sizeof(*sc->visible));

    if (v == NULL && n > 0) {
        return false;
    }

    sc->visible = v;
    sc->nwin = n;
    return true;
}

bool scene_set_menus(struct scene *sc, int n)
{
    struct sc_menu *m = grow(sc->menu, &sc->capmenu, n, sizeof(*sc->menu));

    if (m == NULL && n > 0) {
        return false;
    }

    sc->menu = m;
    sc->nmenu = n;
    return true;
}

/* `a` with the box (bx0, by0)-(bx1, by1) cut out, appended to `out` as up
 * to four rectangles in bands - above, below, then left and right of what
 * is left - so they never overlap (`subtract_into`). */
static void subtract_into(struct sc_rect *out, int *n, const struct sc_rect *a,
                          long bx0, long by0, long bx1, long by1)
{
    long ax1 = a->x + a->w, ay1 = a->y + a->h;
    long y0, y1;

    if (by0 > a->y) {
        out[(*n)++] = (struct sc_rect){ a->x, a->y, a->w, by0 - a->y };
    }

    if (by1 < ay1) {
        out[(*n)++] = (struct sc_rect){ a->x, by1, a->w, ay1 - by1 };
    }

    y0 = by0 > a->y ? by0 : a->y;
    y1 = by1 < ay1 ? by1 : ay1;

    if (y1 > y0) {
        if (bx0 > a->x) {
            out[(*n)++] = (struct sc_rect){ a->x, y0, bx0 - a->x, y1 - y0 };
        }

        if (bx1 < ax1) {
            out[(*n)++] = (struct sc_rect){ bx1, y0, ax1 - bx1, y1 - y0 };
        }
    }
}

/* What the round corners of a window leave uncovered inside the box it
 * covers: the four corner squares, clipped to it (`OUT.uncover`). */
static void uncover(struct sc_rect *out, int *n, const struct sc_window *w, long c,
                    long x0, long y0, long x1, long y1)
{
    long at[4][2] = {
        { w->x, w->y }, { w->x + w->w - c, w->y },
        { w->x, w->y + w->h - c }, { w->x + w->w - c, w->y + w->h - c },
    };
    int k;

    for (k = 0; k < 4; k++) {
        long ix0 = at[k][0] > x0 ? at[k][0] : x0;
        long iy0 = at[k][1] > y0 ? at[k][1] : y0;
        long ix1 = at[k][0] + c < x1 ? at[k][0] + c : x1;
        long iy1 = at[k][1] + c < y1 ? at[k][1] + c : y1;

        if (ix1 > ix0 && iy1 > iy0) {
            out[(*n)++] = (struct sc_rect){ ix0, iy0, ix1 - ix0, iy1 - iy0 };
        }
    }
}

/* The four corners of a frame inside `r`: where each lies on the back and
 * where it is kept, `x y w h kx ky` (`OUT.corners`). */
static int corners(const struct scene *sc, long fx, long fy, long fw, long fh,
                   const struct sc_rect *r, long out[4][6])
{
    long c = sc->corner;
    long at[4][4] = {
        { fx, fy, 0, 0 }, { fx + fw - c, fy, c, 0 },
        { fx, fy + fh - c, 0, c }, { fx + fw - c, fy + fh - c, c, c },
    };
    int k, n = 0;

    if (c <= 0 || c > 32) {
        return 0;
    }

    for (k = 0; k < 4; k++) {
        long x0 = at[k][0] > r->x ? at[k][0] : r->x;
        long y0 = at[k][1] > r->y ? at[k][1] : r->y;
        long x1 = at[k][0] + c < r->x + r->w ? at[k][0] + c : r->x + r->w;
        long y1 = at[k][1] + c < r->y + r->h ? at[k][1] + c : r->y + r->h;

        if (x1 > x0 && y1 > y0) {
            out[n][0] = x0;
            out[n][1] = y0;
            out[n][2] = x1 - x0;
            out[n][3] = y1 - y0;
            out[n][4] = at[k][2] + (x0 - at[k][0]);
            out[n][5] = at[k][3] + (y0 - at[k][1]);
            n++;
        }
    }

    return n;
}

static void keep_corners(struct scene *sc, long kept[4][6], int n)
{
    int k;

    for (k = 0; k < n; k++) {
        blit_on(&sc->keep, sc->back, kept[k][0], kept[k][1], kept[k][2],
                kept[k][3], kept[k][4], kept[k][5]);
    }
}

static void put_back(struct scene *sc, long kept[4][6], int n,
                     long fx, long fy, long fw, long fh)
{
    int k;

    for (k = 0; k < n; k++) {
        blit_round_on(sc->back, &sc->keep, kept[k][4], kept[k][5], kept[k][2],
                      kept[k][3], kept[k][0], kept[k][1], fx, fy, fw, fh,
                      sc->corner, 1);
    }
}

/* The desktop under what no window covers (`draw_desktop`). */
static void draw_desktop(struct scene *sc, const struct sc_rect *r)
{
    bool covered = sc->wallpaper != NULL
                   && r->x >= sc->wx && r->y >= sc->wy
                   && r->x + r->w <= sc->wx + sc->ww
                   && r->y + r->h <= sc->wy + sc->wh;

    if (!covered) {
        fill_on(sc->back, r->x, r->y, r->w, r->h, sc->desktop);
    }

    if (sc->wallpaper != NULL) {
        blit_on(sc->back, sc->wallpaper, r->x - sc->wx, r->y - sc->wy, r->w, r->h,
                r->x, r->y);
    }

    if (sc->stamp != NULL) {
        long sw = (long)sc->stamp->width, sh = (long)sc->stamp->height;
        long x0 = r->x > sc->sx ? r->x : sc->sx;
        long y0 = r->y > sc->sy ? r->y : sc->sy;
        long x1 = r->x + r->w < sc->sx + sw ? r->x + r->w : sc->sx + sw;
        long y1 = r->y + r->h < sc->sy + sh ? r->y + r->h : sc->sy + sh;

        if (x1 > x0 && y1 > y0) {
            blit_on(sc->back, sc->stamp, x0 - sc->sx, y0 - sc->sy, x1 - x0, y1 - y0,
                    x0, y0);
        }
    }
}

/* One light (`OUT.light`): a disc in its colours, and its glyph when the
 * pointer is over the three. */
static void light(struct scene *sc, long x, long y, int slot, bool lit, bool off,
                  bool held)
{
    long d = sc->d;
    long dx = x + (sc->box - d) / 2, dy = y + (sc->box - d) / 2;
    long cx, cy, k, i;

    if (off) {
        fill_round_on(sc->back, dx, dy, d, d, sc->track, d / 2);
        frame_round_on(sc->back, dx, dy, d, d, sc->line_soft, d / 2);
        return;
    }

    fill_round_on(sc->back, dx, dy, d, d, held ? sc->held_fill[slot] : sc->fill[slot],
                  d / 2);
    frame_round_on(sc->back, dx, dy, d, d, sc->ring[slot], d / 2);

    if (!lit) {
        return;
    }

    cx = dx + d / 2;
    cy = dy + d / 2;
    k = d / 4;

    if (slot == 1) {                                    /* minimise */
        fill_on(sc->back, cx - k - 1, cy - 1, 2 * k + 2, 2, sc->mark[slot]);
    } else if (slot == 2) {                             /* close */
        for (i = -k; i <= k; i++) {
            fill_on(sc->back, cx + i - 1, cy + i - 1, 2, 2, sc->mark[slot]);
            fill_on(sc->back, cx - i - 1, cy + i - 1, 2, 2, sc->mark[slot]);
        }
    } else {                                            /* maximise */
        double ax[3] = { (double)(cx - k - 1), (double)(cx + k), (double)(cx + k) };
        double ay[3] = { (double)(cy - k), (double)(cy - k), (double)(cy + k - 1) };
        double bx[3] = { (double)(cx - k), (double)(cx - k), (double)(cx + k - 1) };
        double by[3] = { (double)(cy - k + 1), (double)(cy + k), (double)(cy + k) };

        triangle_on(sc->back, ax, ay, sc->mark[slot]);
        triangle_on(sc->back, bx, by, sc->mark[slot]);
    }
}

/* A window's part of `r` (what `drawwindow.lua` did, before D3b-2). */
/*
 * **A picture from before a resize** (`SC_STALE`): shown at its own size in
 * the frame's top left, and the new room beyond it filled by drawing the
 * picture's last column across to the right and its last row down - which
 * is the window's own background nearly always - until the application
 * draws at the new size. It was stretched to the frame, and every window
 * being resized swelled for a moment and then snapped back (Diego, on the
 * M700, 10 October 2026: "it looks that the app streches to the new
 * dimensions and then redraws in the new size which looks weird").
 *
 * `top` and `tall` are where the picture's part of the frame begins and
 * how tall it is - below a menu bar, when there is one; `x0`..`y1` the
 * rectangle being composed, which nothing here draws outside.
 */
static void stale_into(struct scene *sc, const struct sc_window *w, long top, long tall,
                       long x0, long y0, long x1, long y1)
{
    long pw = w->src_w < w->w ? w->src_w : w->w;
    long ph = w->src_h < tall ? w->src_h : tall;

    if (pw <= 0 || ph <= 0) {
        return;
    }

    /* The picture as it is, where it overlaps the rectangle. */
    {
        long ax0 = x0 > w->x ? x0 : w->x;
        long ay0 = y0 > top ? y0 : top;
        long ax1 = x1 < w->x + pw ? x1 : w->x + pw;
        long ay1 = y1 < top + ph ? y1 : top + ph;

        if (ax1 > ax0 && ay1 > ay0) {
            blit_on(sc->back, w->picture, ax0 - w->x, ay0 - top, ax1 - ax0, ay1 - ay0,
                    ax0, ay0);
        }
    }

    /* Its last column, across the room to the right. */
    if (w->w > pw) {
        stretch_into(sc->back, w->picture, pw - 1, 0, 1, ph,
                     w->x + pw, top, w->w - pw, ph, -1, false, x0, y0, x1, y1);
    }

    /* Its last row, down the room below, column for column. */
    if (tall > ph) {
        stretch_into(sc->back, w->picture, 0, ph - 1, pw, 1,
                     w->x, top + ph, pw, tall - ph, -1, false, x0, y0, x1, y1);
    }

    /* And the corner beyond both, its last pixel. */
    if (w->w > pw && tall > ph) {
        stretch_into(sc->back, w->picture, pw - 1, ph - 1, 1, 1,
                     w->x + pw, top + ph, w->w - pw, tall - ph, -1, false,
                     x0, y0, x1, y1);
    }
}

static void draw_window(struct scene *sc, const struct sc_window *w, const struct sc_rect *r)
{
    long kept[4][6];
    int nkept = 0;
    long x0, y0, x1, y1;

    if ((w->flags & SC_HIDDEN) || w->x >= r->x + r->w || w->x + w->w <= r->x
        || w->y >= r->y + r->h || w->y + w->h <= r->y) {
        return;
    }

    if (w->flags & SC_ROUNDED) {
        nkept = corners(sc, w->x, w->y, w->w, w->h, r, kept);
        keep_corners(sc, kept, nkept);
    }

    x0 = w->x > r->x ? w->x : r->x;
    y0 = w->y > r->y ? w->y : r->y;
    x1 = w->x + w->w < r->x + r->w ? w->x + w->w : r->x + r->w;
    y1 = w->y + w->h < r->y + r->h ? w->y + w->h : r->y + r->h;

    if (x1 > x0 && y1 > y0 && w->picture != NULL) {
        if (w->flags & SC_BLEND) {
            blend_on(sc->back, w->picture, x0 - w->x, y0 - w->y, x1 - x0, y1 - y0,
                     x0, y0, 255);
        } else if (w->bar != NULL) {
            /* A menu bar above the picture (`strips.compose`). */
            long split = w->y + w->bar_h;

            if (y0 < split) {
                long yb = y1 < split ? y1 : split;

                blit_on(sc->back, w->bar, x0 - w->x, y0 - w->y, x1 - x0, yb - y0, x0, y0);
            }

            if (y1 > split) {
                long ya = y0 > split ? y0 : split;
                long lower = w->h - w->bar_h;

                if (w->flags & SC_STALE) {
                    stale_into(sc, w, split, lower, x0, ya, x1, y1);
                } else if (w->src_w != w->w || w->src_h != lower) {
                    stretch_into(sc->back, w->picture, 0, 0, w->src_w, w->src_h,
                                 w->x, split, w->w, lower, -1, false,
                                 x0, ya, x1, y1);
                } else {
                    blit_on(sc->back, w->picture, x0 - w->x, ya - split, x1 - x0, y1 - ya,
                            x0, ya);
                }
            }
        } else if (w->flags & SC_STALE) {
            stale_into(sc, w, w->y, w->h, x0, y0, x1, y1);
        } else if (w->src_w != w->w || w->src_h != w->h) {
            stretch_into(sc->back, w->picture, 0, 0, w->src_w, w->src_h,
                         w->x, w->y, w->w, w->h, -1, false, x0, y0, x1, y1);
        } else {
            blit_on(sc->back, w->picture, x0 - w->x, y0 - w->y, x1 - x0, y1 - y0, x0, y0);
        }
    }

    /* The sizing grip, three diagonal steps over its bottom-right corner. */
    if (w->flags & SC_GRIP) {
        long gx = w->x + w->w - sc->grip, gy = w->y + w->h - sc->grip;
        long step;

        for (step = 0; step <= 2; step++) {
            long o = step * 5, n = sc->grip - 3 - o;

            if (n > 0) {
                fill_on(sc->back, gx + o + 2, gy + sc->grip - 3 - o, n, 1, sc->edge_light);
                fill_on(sc->back, gx + o + 2, gy + sc->grip - 2 - o, n, 1, sc->edge_dark);
            }
        }
    }

    /* The three, over its header. */
    if ((w->flags & SC_LIGHTS) && w->bx < r->x + r->w && w->bx + sc->run > r->x
        && w->by < r->y + r->h && w->by + sc->box > r->y) {
        int slot;

        for (slot = 0; slot < SC_SLOTS; slot++) {
            bool held = w->held == slot;
            bool off = !(w->flags & SC_FOCUSED) || (slot == 0 && !(w->flags & SC_MAXIMISE));

            light(sc, w->bx + sc->box_w * slot, w->by, slot,
                  (w->flags & SC_LIT) || held, off, held);
        }
    }

    if (nkept > 0) {
        put_back(sc, kept, nkept, w->x, w->y, w->w, w->h);
    }
}

/* A window's shadow, under it (`OUT.cast_shadow`). */
static void cast_shadow(struct scene *sc, const struct sc_window *w, const struct sc_rect *r)
{
    long s = sc->shadow;
    long sx, sy, sw, sh;
    long clip[4];

    if (s <= 0 || !(w->flags & SC_SHADOW) || (w->flags & SC_HIDDEN)) {
        return;
    }

    sx = w->x - s;
    sy = w->y - s + s / 3;
    sw = w->w + s * 2;
    sh = w->h + s * 2;

    if (sx >= r->x + r->w || sx + sw <= r->x || sy >= r->y + r->h || sy + sh <= r->y) {
        return;
    }

    clip[0] = r->x;
    clip[1] = r->y;
    clip[2] = r->w;
    clip[3] = r->h;
    gfx_shadow(sc->back->pixels, sc->back->pitch, (long)sc->back->width,
               (long)sc->back->height, w->x, w->y, w->w, w->h, sc->corner, s,
               (w->flags & SC_DIM) ? 55 : 90, clip);
}

/* Both lists of pieces at least `want` long; they keep what they hold.
 * False when there is no memory. */
static bool pieces_room(struct scene *sc, int want)
{
    int n = sc->cap_pieces > 0 ? sc->cap_pieces : 64;
    struct sc_rect *a, *b;

    if (want <= sc->cap_pieces && sc->pieces[0] != NULL) {
        return true;
    }

    while (n < want) {
        n *= 2;
    }

    a = realloc(sc->pieces[0], (size_t)n * sizeof(struct sc_rect));

    if (a == NULL) {
        return false;
    }

    sc->pieces[0] = a;
    b = realloc(sc->pieces[1], (size_t)n * sizeof(struct sc_rect));

    if (b == NULL) {
        return false;
    }

    sc->pieces[1] = b;
    sc->cap_pieces = n;
    return true;
}

/* One damaged rectangle, composed into the back buffer; how many pixels
 * were drawn into it, for `frames`. */
long scene_compose(struct scene *sc, long rx, long ry, long rw, long rh)
{
    struct sc_rect r = { rx, ry, rw, rh };
    int cur = 0, nrem = 1, nkeep, k, p;
    struct sc_rect *remaining, *keep;
    long drawn = 0;

    if (sc->back == NULL || sc->back->pixels == NULL || r.w <= 0 || r.h <= 0) {
        return 0;
    }

    if (!pieces_room(sc, 1)) {
        return -1;
    }

    sc->pieces[cur][0] = r;

    /* Front to back: what each window shows of what is left uncovered. */
    for (k = sc->nwin - 1; k >= 0; k--) {
        struct sc_window *w = &sc->win[k];
        struct sc_rect *v = &sc->visible[k];
        long c = sc->corner;
        bool round = (w->flags & SC_ROUNDED) && c > 0 && c <= 32;
        bool mine = false;

        v->w = 0;

        if (nrem == 0 || (w->flags & SC_HIDDEN)) {
            continue;
        }

        /* A piece becomes at most eight: four around the window, and the
         * four corner squares its rounding leaves showing. */
        if (!pieces_room(sc, nrem * 8)) {
            return -1;
        }

        remaining = sc->pieces[cur];
        keep = sc->pieces[1 - cur];
        nkeep = 0;

        for (p = 0; p < nrem; p++) {
            struct sc_rect *piece = &remaining[p];
            long x0 = w->x > piece->x ? w->x : piece->x;
            long y0 = w->y > piece->y ? w->y : piece->y;
            long x1 = w->x + w->w < piece->x + piece->w ? w->x + w->w : piece->x + piece->w;
            long y1 = w->y + w->h < piece->y + piece->h ? w->y + w->h : piece->y + piece->h;

            if (x1 > x0 && y1 > y0) {
                if (mine) {
                    long vx1 = v->x + v->w, vy1 = v->y + v->h;

                    if (x0 < v->x) v->x = x0;
                    if (y0 < v->y) v->y = y0;
                    if (x1 > vx1) vx1 = x1;
                    if (y1 > vy1) vy1 = y1;
                    v->w = vx1 - v->x;
                    v->h = vy1 - v->y;
                } else {
                    *v = (struct sc_rect){ x0, y0, x1 - x0, y1 - y0 };
                    mine = true;
                }

                if (w->flags & SC_BLEND) {
                    keep[nkeep++] = *piece;
                } else {
                    subtract_into(keep, &nkeep, piece, x0, y0, x1, y1);

                    if (round) {
                        uncover(keep, &nkeep, w, c, x0, y0, x1, y1);
                    }
                }
            } else {
                keep[nkeep++] = *piece;
            }
        }

        cur = 1 - cur;
        nrem = nkeep;
    }

    remaining = sc->pieces[cur];

    for (p = 0; p < nrem; p++) {
        drawn += (long)remaining[p].w * remaining[p].h;
        draw_desktop(sc, &remaining[p]);
    }

    /* Back to front: each window's shadow, then what it shows. */
    for (k = 0; k < sc->nwin; k++) {
        cast_shadow(sc, &sc->win[k], &r);

        if (sc->visible[k].w > 0) {
            drawn += (long)sc->visible[k].w * sc->visible[k].h;
            draw_window(sc, &sc->win[k], &sc->visible[k]);
        }
    }

    /* Menus, above every window, rounded as a window is. */
    for (k = 0; k < sc->nmenu; k++) {
        struct sc_menu *m = &sc->menu[k];
        long x0 = m->x > r.x ? m->x : r.x;
        long y0 = m->y > r.y ? m->y : r.y;
        long x1 = m->x + m->w < r.x + r.w ? m->x + m->w : r.x + r.w;
        long y1 = m->y + m->h < r.y + r.h ? m->y + m->h : r.y + r.h;
        long kept[4][6];
        int nkept;

        if (x1 <= x0 || y1 <= y0 || m->picture == NULL) {
            continue;
        }

        nkept = corners(sc, m->x, m->y, m->w, m->h, &r, kept);
        keep_corners(sc, kept, nkept);
        blit_on(sc->back, m->picture, x0 - m->x, y0 - m->y, x1 - x0, y1 - y0, x0, y0);
        put_back(sc, kept, nkept, m->x, m->y, m->w, m->h);
    }

    /* The rubber band. */
    if (sc->outline_on) {
        long o = sc->outline;

        fill_on(sc->back, sc->ox, sc->oy, sc->ow, o, sc->outline_colour);
        fill_on(sc->back, sc->ox, sc->oy + sc->oh - o, sc->ow, o, sc->outline_colour);
        fill_on(sc->back, sc->ox, sc->oy, o, sc->oh, sc->outline_colour);
        fill_on(sc->back, sc->ox + sc->ow - o, sc->oy, o, sc->oh, sc->outline_colour);
    }

    /* The level bar. */
    if (sc->osd != NULL && sc->osd_alpha > 0) {
        long ow = (long)sc->osd->width, oh = (long)sc->osd->height;
        long x0 = r.x > sc->osd_x ? r.x : sc->osd_x;
        long y0 = r.y > sc->osd_y ? r.y : sc->osd_y;
        long x1 = r.x + r.w < sc->osd_x + ow ? r.x + r.w : sc->osd_x + ow;
        long y1 = r.y + r.h < sc->osd_y + oh ? r.y + r.h : sc->osd_y + oh;

        if (x1 > x0 && y1 > y0) {
            blend_on(sc->back, sc->osd, x0 - sc->osd_x, y0 - sc->osd_y, x1 - x0, y1 - y0,
                     x0, y0, sc->osd_alpha);
        }
    }

    /* A panel over every window - the app switcher - by its own alpha. */
    if (sc->panel != NULL) {
        long pw = (long)sc->panel->width, ph = (long)sc->panel->height;
        long x0 = r.x > sc->panel_x ? r.x : sc->panel_x;
        long y0 = r.y > sc->panel_y ? r.y : sc->panel_y;
        long x1 = r.x + r.w < sc->panel_x + pw ? r.x + r.w : sc->panel_x + pw;
        long y1 = r.y + r.h < sc->panel_y + ph ? r.y + r.h : sc->panel_y + ph;

        if (x1 > x0 && y1 > y0) {
            blend_on(sc->back, sc->panel, x0 - sc->panel_x, y0 - sc->panel_y, x1 - x0, y1 - y0,
                     x0, y0, 255);
        }
    }

    /* The pointer, where the display does not draw it. */
    if (sc->cursor != NULL) {
        blend_on(sc->back, sc->cursor, 0, 0, (long)sc->cursor->width,
                 (long)sc->cursor->height, sc->cursor_x, sc->cursor_y, 255);
    }

    /* And a drag's label beside it, clipped to the rectangle like the rest. */
    if (sc->badge != NULL) {
        long bw = (long)sc->badge->width, bh = (long)sc->badge->height;
        long x0 = r.x > sc->badge_x ? r.x : sc->badge_x;
        long y0 = r.y > sc->badge_y ? r.y : sc->badge_y;
        long x1 = r.x + r.w < sc->badge_x + bw ? r.x + r.w : sc->badge_x + bw;
        long y1 = r.y + r.h < sc->badge_y + bh ? r.y + r.h : sc->badge_y + bh;

        if (x1 > x0 && y1 > y0) {
            blit_on(sc->back, sc->badge, x0 - sc->badge_x, y0 - sc->badge_y, x1 - x0, y1 - y0,
                    x0, y0);
        }
    }

    return drawn;
}
