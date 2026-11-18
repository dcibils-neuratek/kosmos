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
#include "blur.h"

void scene_init(struct scene *sc, struct surface *back)
{
    memset(sc, 0, sizeof(*sc));
    sc->back = back;
    sc->keep.pixels = sc->keep_pixels;
    sc->keep.width = sc->keep.height = 64;
    sc->keep.pitch = 64 * 4;
    sc->outline = 2;
    sc->live_blur = -1;
}

void scene_free(struct scene *sc)
{
    free(sc->win);
    free(sc->menu);
    free(sc->pieces[0]);
    free(sc->pieces[1]);
    free(sc->visible);
    free(sc->lens_mem);
    sc->lens_mem = NULL;
    sc->lens_ok = false;
    for (unsigned i = 0; i < sizeof sc->rims / sizeof sc->rims[0]; i++) {
        free(sc->rims[i].mem);
        sc->rims[i].mem = NULL;
        sc->rims[i].ok = false;
    }
    free(sc->live_mem);
    sc->live_mem = NULL;
    sc->live_cap = 0;
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
                   const struct sc_rect *r, long out[4][6], long c)
{
    (void)sc;
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

/*
 * What the cut leaves outside a frame's top-left corner, put back from the
 * square kept from under it: as much of each pixel as the frame does not
 * cover (`gfx_cut_cover`), the two meetings eased by a circle seven eighths
 * of the cut - `docs/astra.html`'s menu, cut at 8 and eased over 3 pixels
 * either side of each meeting.
 */
static void cut_back(struct scene *sc, const long kept[6], long fx, long fy, long cut)
{
    long ease = cut * 7 / 8;

    for (long row = 0; row < kept[3]; row++) {
        long y = kept[1] + row;

        if (y < 0 || y >= (long)sc->back->height) continue;

        for (long col = 0; col < kept[2]; col++) {
            long x = kept[0] + col;
            long cover = 255 - gfx_cut_cover(x, y, fx, fy, cut, ease);
            uint32_t *dp, sp;

            if (cover <= 0 || x < 0 || x >= (long)sc->back->width) continue;

            dp = (uint32_t *)((uint8_t *)sc->back->pixels + (size_t)y * sc->back->pitch) + x;
            sp = *((uint32_t *)((uint8_t *)sc->keep.pixels
                                + (size_t)(kept[5] + row) * sc->keep.pitch) + kept[4] + col);

            if (cover >= 255) {
                *dp = sp;
            } else {
                /* The kept pixel over the frame's, by how much is uncovered. */
                uint32_t a = (uint32_t)cover, b = 255 - a, d = *dp, out = 0xff000000u;

                for (int sh = 0; sh < 24; sh += 8) {
                    uint32_t v = (((sp >> sh) & 0xffu) * a + ((d >> sh) & 0xffu) * b + 127) / 255;

                    out |= v << sh;
                }

                *dp = out;
            }
        }
    }
}

static void put_back(struct scene *sc, long kept[4][6], int n,
                     long fx, long fy, long fw, long fh, long size, long cut)
{
    int k;

    for (k = 0; k < n; k++) {
        /*
         * **The top-left of a cut frame** (Starlight's, `docs/astra.md` S2):
         * kept from the first of the four squares, and what lies outside
         * the cut put back - the eased chamfer instead of the arc.
         */
        if (cut > 0 && kept[k][4] < size && kept[k][5] < size) {
            cut_back(sc, kept[k], fx, fy, cut);
            continue;
        }

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
/*
 * **A glass bead** (Liquid Glass L5): under a look with a rim, a round
 * button is a bead of its colour's glass - its highlight a smaller round of
 * white at its upper left, as light from there catches a drop. Grey glass
 * behind, its highlight fainter; pressed, fainter too. `alpha` is the
 * highlight's. Nothing without a rim: the light is the glass's.
 */
static void bead(struct scene *sc, long dx, long dy, long d, unsigned alpha)
{
    long hw = d / 2, hh = d * 2 / 5;

    if (sc->rim_bevel <= 0 || hw < 2 || hh < 2) {
        return;
    }

    fill_round_on(sc->back, dx + d / 5, dy + d / 10, hw, hh,
                  (uint32_t)alpha << 24 | 0xffffffu, hh / 2);
}

static void light(struct scene *sc, long x, long y, int slot, bool lit, bool off,
                  bool held)
{
    long d = sc->d;
    long dx = x + (sc->box - d) / 2, dy = y + (sc->box - d) / 2;
    long cx, cy, k, i;

    if (off) {
        fill_round_on(sc->back, dx, dy, d, d, sc->track, d / 2);
        frame_round_on(sc->back, dx, dy, d, d, sc->line_soft, d / 2);
        bead(sc, dx, dy, d, 0x40);
        return;
    }

    fill_round_on(sc->back, dx, dy, d, d, held ? sc->held_fill[slot] : sc->fill[slot],
                  d / 2);
    frame_round_on(sc->back, dx, dy, d, d, sc->ring[slot], d / 2);
    bead(sc, dx, dy, d, held ? 0x50 : 0x90);

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

/*
 * **One of Platinum's boxes** (`OUT.platinum_box`, `docs/pewter.md` P2): a
 * square with softened corners in the look's colours for its slot, and its
 * mark always shown, as Mac OS drew them - close is the plain box, zoom a
 * smaller square inside it, collapse two lines across. Nothing at all on a
 * window that is not in front: Platinum takes the boxes off a window behind
 * rather than greying them.
 */
static void platinum_box(struct scene *sc, long x, long y, int slot, bool held)
{
    long d = sc->d;
    long dx = x + (sc->box - d) / 2, dy = y + (sc->box - d) / 2;
    long r = d / 5, k = d / 4;

    fill_round_on(sc->back, dx, dy, d, d, held ? sc->held_fill[slot] : sc->fill[slot], r);
    frame_round_on(sc->back, dx, dy, d, d, sc->ring[slot], r);

    if (slot == 0) {                                    /* zoom */
        long s = d - 2 * k;

        frame_round_on(sc->back, dx + k, dy + k, s, s, sc->mark[slot], 1);
    } else if (slot == 1) {                             /* collapse */
        fill_on(sc->back, dx + k - 1, dy + d / 2 - 2, d - 2 * k + 2, 1, sc->mark[slot]);
        fill_on(sc->back, dx + k - 1, dy + d / 2 + 1, d - 2 * k + 2, 1, sc->mark[slot]);
    }
}

/* Where a window's box in `slot` is, across its title bar. */
static long box_left(const struct scene *sc, const struct sc_window *w, int slot)
{
    if (w->flags & SC_PLATINUM) {
        return slot == 2 ? w->bx : w->bx2 + sc->box_w * slot;
    }

    return w->bx + sc->box_w * slot;
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
                       long global, long x0, long y0, long x1, long y1)
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

        if (ax1 > ax0 && ay1 > ay0 && global < 0) {
            blit_on(sc->back, w->picture, ax0 - w->x, ay0 - top, ax1 - ax0, ay1 - ay0,
                    ax0, ay0);
        } else if (ax1 > ax0 && ay1 > ay0) {
            blend_on(sc->back, w->picture, ax0 - w->x, ay0 - top, ax1 - ax0, ay1 - ay0,
                     ax0, ay0, (unsigned)global);
        }
    }

    /* Its last column, across the room to the right. */
    if (w->w > pw) {
        stretch_into(sc->back, w->picture, pw - 1, 0, 1, ph,
                     w->x + pw, top, w->w - pw, ph, global, false, x0, y0, x1, y1);
    }

    /* Its last row, down the room below, column for column. */
    if (tall > ph) {
        stretch_into(sc->back, w->picture, 0, ph - 1, pw, 1,
                     w->x, top + ph, pw, tall - ph, global, false, x0, y0, x1, y1);
    }

    /* And the corner beyond both, its last pixel. */
    if (w->w > pw && tall > ph) {
        stretch_into(sc->back, w->picture, pw - 1, ph - 1, 1, 1,
                     w->x + pw, top + ph, w->w - pw, tall - ph, global, false,
                     x0, y0, x1, y1);
    }
}

/*
 * **A window's inset** (`sc_window.inset`), over its picture: where the
 * picture's pixels `ix`..`ix + iw` land on the screen, worked out as the
 * picture's are - at its own size when it is stale or not scaled, and
 * stretched with it when the window is at another scale than its pixels.
 * `top` and `tall` are the picture's part of the frame, as `stale_into`
 * has them; `x0`..`y1` the rectangle being composed.
 */
void scene_inset_place(const struct sc_window *w, long top, long tall,
                       long *dx, long *dy, long *dw, long *dh)
{
    if ((w->flags & SC_STALE) || (w->src_w == w->w && w->src_h == tall)
        || w->src_w <= 0 || w->src_h <= 0) {
        *dx = w->x + w->ix;
        *dy = top + w->iy;
        *dw = w->iw;
        *dh = w->ih;
        return;
    }

    *dx = w->x + w->ix * w->w / w->src_w;
    *dy = top + w->iy * tall / w->src_h;
    *dw = w->x + (w->ix + w->iw) * w->w / w->src_w - *dx;
    *dh = top + (w->iy + w->ih) * tall / w->src_h - *dy;
}

static void inset_into(struct scene *sc, const struct sc_window *w, long top, long tall,
                       long x0, long y0, long x1, long y1)
{
    long dx, dy, dw, dh, ax0, ay0, ax1, ay1;

    if (w->inset == NULL || w->iw <= 0 || w->ih <= 0) {
        return;
    }

    scene_inset_place(w, top, tall, &dx, &dy, &dw, &dh);

    /* Never past the picture's part of the frame, nor the rectangle. */
    if (y0 < top) {
        y0 = top;
    }
    if (y1 > top + tall) {
        y1 = top + tall;
    }

    ax0 = x0 > dx ? x0 : dx;
    ay0 = y0 > dy ? y0 : dy;
    ax1 = x1 < dx + dw ? x1 : dx + dw;
    ay1 = y1 < dy + dh ? y1 : dy + dh;

    if (ax1 <= ax0 || ay1 <= ay0) {
        return;
    }

    if (dw == w->iw && dh == w->ih) {
        blit_on(sc->back, w->inset, ax0 - dx, ay0 - dy, ax1 - ax0, ay1 - ay0, ax0, ay0);
    } else {
        stretch_into(sc->back, w->inset, 0, 0, w->iw, w->ih, dx, dy, dw, dh, -1, false,
                     ax0, ay0, ax1, ay1);
    }
}

/*
 * **Glass under a window** (`SC_GLASS`, Liquid Glass LG4): wherever its
 * picture draws anything at all - alpha above nought - the wallpaper's
 * glass, from where the wallpaper lies, or the desktop's colour past it;
 * then the picture is blended over that, as any blended window is. So the
 * dock's tint frosts the wallpaper behind it, and what is under the dock -
 * a window - is not seen through it, which is wallpaper glass's one
 * honest difference from real glass (`docs/astra.md`, Liquid Glass 1).
 * Where it draws nothing, its rounded ends, what is under it shows.
 *
 * Of a picture whose top-left is at `px`, `py` on the screen: a window's,
 * or a menu's.
 *
 * **And its rim**, when `rim` and the scene has a lens (LG3): within the
 * bevel of the surface's rounded edge - `pw` by `ph`, rounded at the
 * scene's corner, which is the radius the lens was made for - the glass is
 * read from where the lens bends the pixel to (`docs/astra.md`, Liquid
 * Glass 3). Its light is not added here but over the picture, after it is
 * blended (`rim_light`): the highlight is on the glass's surface, and a
 * window's tint over it buried it.
 */
static struct gfx_lens_px lens_at(const struct gfx_lens *l, long lx, long ly, long pw, long ph)
{
    static const struct gfx_lens_px none = { 0, 0, 0 };
    long s = (long)l->side, b = (long)l->bevel;
    long rx = pw - 1 - lx, by = ph - 1 - ly;

    if (lx >= b && rx >= b && ly >= b && by >= b
        && (lx >= s || ly >= s) && (rx >= s || ly >= s)
        && (rx >= s || by >= s) && (lx >= s || by >= s)) {
        return none;
    }

    if (lx < s && ly < s) return l->corner[GFX_LENS_TL][ly * s + lx];
    if (rx < s && ly < s) return l->corner[GFX_LENS_TR][ly * s + rx];
    if (rx < s && by < s) return l->corner[GFX_LENS_BR][by * s + rx];
    if (lx < s && by < s) return l->corner[GFX_LENS_BL][by * s + lx];
    if (ly < b) return l->edge[GFX_LENS_TOP][ly];
    if (by < b) return l->edge[GFX_LENS_BOTTOM][by];
    if (lx < b) return l->edge[GFX_LENS_LEFT][lx];
    if (rx < b) return l->edge[GFX_LENS_RIGHT][rx];
    return none;
}

static uint32_t glass_px(const struct scene *sc, long gx, long gy)
{
    const struct surface *g = sc->glass;

    if (gx < 0 || gy < 0 || gx >= (long)g->width || gy >= (long)g->height) {
        if (!sc->glass_clamp) return sc->desktop;
        gx = gx < 0 ? 0 : gx >= (long)g->width ? (long)g->width - 1 : gx;
        gy = gy < 0 ? 0 : gy >= (long)g->height ? (long)g->height - 1 : gy;
    }

    return row_of(g, (unsigned)gy)[gx];
}

/* Light of colour `toward` added by `spec` of 255: each channel toward
 * the light's by that share - white for a cool rim, the look's
 * `rim_front` for the window in front's warm one. */
static uint32_t lit(uint32_t c, unsigned spec, uint32_t toward)
{
    uint32_t out = 0xff000000u;
    unsigned k;

    for (k = 0; k < 24; k += 8) {
        int v = (int)((c >> k) & 0xffu);
        int t = (int)((toward >> k) & 0xffu);

        out |= (uint32_t)(v + ((t - v) * (int)spec + (t >= v ? 127 : -127)) / 255) << k;
    }

    return out;
}

/*
 * **A rim's shape**: the lens and the rounded rectangle on the screen it
 * bends and lights - a window's or a menu's own, with the scene's lens for
 * its corner, or a strip's glass inside its picture (`sc_window.rr`) with
 * a lens for that radius. `lens` NULL for none: flat glass, no light.
 */
struct rim_shape {
    const struct gfx_lens *lens;
    long x, y, w, h;
};

static void glass_under(struct scene *sc, const struct surface *picture, long px, long py,
                        const struct rim_shape *shape,
                        long x0, long y0, long x1, long y1)
{
    const struct surface *g = sc->glass;
    const struct gfx_lens *l = shape != NULL ? shape->lens : NULL;
    long x, y;

    for (y = y0; y < y1; y++) {
        const uint32_t *p = row_of(picture, (unsigned)(y - py));
        uint32_t *o = row_of(sc->back, (unsigned)y);
        long gy = y - sc->wy;
        bool in_y = gy >= 0 && gy < (long)g->height;
        const uint32_t *gr = in_y ? row_of(g, (unsigned)gy) : NULL;

        for (x = x0; x < x1; x++) {
            long gx = x - sc->wx;

            if ((p[x - px] >> 24) == 0) continue;

            if (l != NULL && x >= shape->x && x < shape->x + shape->w
                && y >= shape->y && y < shape->y + shape->h) {
                struct gfx_lens_px e = lens_at(l, x - shape->x, y - shape->y,
                                               shape->w, shape->h);

                if (e.dx != 0 || e.dy != 0) {
                    /* **Amber and indigo** (L6): the warm channels read
                     * further along the bend than the cool one, by the
                     * look's `split`, so an edge fringes in Kosmos's two
                     * colours rather than a rainbow. */
                    long s = sc->rim_split;
                    long wx = e.dx * (100 + s) / 100, wy = e.dy * (100 + s) / 100;
                    long cx = e.dx * (100 - s) / 100, cy = e.dy * (100 - s) / 100;
                    uint32_t warm = glass_px(sc, gx + wx, gy + wy);
                    uint32_t cool = glass_px(sc, gx + cx, gy + cy);

                    o[x] = (warm & 0xffffff00u) | (cool & 0xffu);
                    continue;
                }
            }

            o[x] = gr != NULL && gx >= 0 && gx < (long)g->width ? gr[gx] : sc->desktop;
        }
    }
}

/* The wallpaper's glass over a rectangle of the screen, flat: no lens and
 * no picture to say where - a glass window's frame in a resize. */
static void glass_flat(struct scene *sc, long x0, long y0, long x1, long y1)
{
    long x, y;

    for (y = y0; y < y1; y++) {
        uint32_t *o = row_of(sc->back, (unsigned)y);

        for (x = x0; x < x1; x++) o[x] = glass_px(sc, x - sc->wx, y - sc->wy);
    }
}

/*
 * **Live glass** (Liquid Glass LG6, G2): a floating glass surface - `sx`,
 * `sy`, `sw` by `sh` - over what is really under it. Composing goes back
 * to front, so when the surface's turn comes the back buffer already holds
 * every layer under it: its rectangle and the blur's reach around it are
 * copied from there into the scratch and blurred in place - three box
 * passes at `live_blur`, none for clear glass - and the scratch stands in
 * for the wallpaper's glass while the surface is drawn, so `glass_under`
 * and its lens read it exactly as they read that. A menu over the dock
 * blurs the dock, as it should.
 *
 * Only when the rectangle being composed holds the whole of that reach
 * (`scene_widen` makes it so): anything less would blur pixels from a
 * frame before. Past the screen's edge the scratch is clipped, and a lens
 * that bends past the scratch reads its edge pixel. What it does not see
 * is a window above the surface whose pixels are inside the reach - they
 * are from the frame before until it is drawn - which is the blur's edge
 * and nothing more, since that window then covers it.
 *
 * Whether it was: false, and nothing changed, for wallpaper glass.
 */
struct glass_was {
    struct surface *glass;
    long wx, wy;
};

static long live_reach(const struct scene *sc)
{
    return 3 * sc->live_blur + sc->rim_bevel;
}

/* The surface's reach on the screen, `e`; false when it has none. */
static bool live_rect(const struct scene *sc, long sx, long sy, long sw, long sh,
                      struct sc_rect *e)
{
    long reach = live_reach(sc);
    long bw = (long)sc->back->width, bh = (long)sc->back->height;
    long x0 = sx - reach, y0 = sy - reach, x1 = sx + sw + reach, y1 = sy + sh + reach;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > bw) x1 = bw;
    if (y1 > bh) y1 = bh;
    if (x1 <= x0 || y1 <= y0) return false;
    *e = (struct sc_rect){ x0, y0, x1 - x0, y1 - y0 };
    return true;
}

static bool live_under(struct scene *sc, unsigned flags, long sx, long sy, long sw, long sh,
                       struct glass_was *was)
{
    struct sc_rect e;
    const struct sc_rect *r = &sc->cur;
    size_t n, need;
    long y;

    if (!(flags & SC_LIVE) || sc->live_blur < 0 || (unsigned long)sc->live_blur > GFX_BLUR_MOST
        || !live_rect(sc, sx, sy, sw, sh, &e)
        || e.x < r->x || e.y < r->y || e.x + e.w > r->x + r->w || e.y + e.h > r->y + r->h) {
        return false;
    }

    n = (size_t)e.w * (size_t)e.h;
    need = 2 * n + 4 * (size_t)e.w;

    if (need > sc->live_cap) {
        uint32_t *m = realloc(sc->live_mem, need * sizeof(uint32_t));

        if (m == NULL) return false;
        sc->live_mem = m;
        sc->live_cap = need;
    }

    for (y = 0; y < e.h; y++) {
        memcpy(sc->live_mem + (size_t)y * (size_t)e.w,
               row_of(sc->back, (unsigned)(e.y + y)) + e.x, (size_t)e.w * sizeof(uint32_t));
    }

    if (sc->live_blur > 0) {
        gfx_blur(sc->live_mem, sc->live_mem, (size_t)e.w, (unsigned)e.w, (unsigned)e.h,
                 (unsigned)sc->live_blur, 3, sc->live_mem + n, sc->live_mem + 2 * n);
    }

    sc->live_glass = (struct surface){ 0 };
    sc->live_glass.pixels = sc->live_mem;
    sc->live_glass.width = (unsigned)e.w;
    sc->live_glass.height = (unsigned)e.h;
    sc->live_glass.pitch = (unsigned)e.w * 4u;

    was->glass = sc->glass;
    was->wx = sc->wx;
    was->wy = sc->wy;
    sc->glass = &sc->live_glass;
    sc->wx = e.x;
    sc->wy = e.y;
    sc->glass_clamp = true;
    sc->live_made++;
    return true;
}

static void live_done(struct scene *sc, const struct glass_was *was)
{
    sc->glass = was->glass;
    sc->wx = was->wx;
    sc->wy = was->wy;
    sc->glass_clamp = false;
}

/*
 * A floating surface that is live glass: blended, glass and live, and shown
 * as it is. By its flags alone, never its picture: the window manager's
 * own scene, which widens the damage, names pictures by the window
 * server's slots and holds none of them - and asked for a picture, it
 * widened nothing, so the dock was live in the one pass that composed the
 * screen and wallpaper glass in every pass after (18 November).
 */
static bool live_strip(const struct sc_window *w)
{
    return (w->flags & (SC_BLEND | SC_GLASS | SC_LIVE | SC_HIDDEN))
               == (SC_BLEND | SC_GLASS | SC_LIVE)
           && w->src_w == w->w && w->src_h == w->h;
}

int scene_widen(const struct scene *sc, struct sc_rect *r, int n)
{
    int grew = 0, k, i;

    if (sc->live_blur < 0 || (unsigned long)sc->live_blur > GFX_BLUR_MOST || sc->back == NULL) {
        return 0;
    }

    for (k = 0; k < sc->nwin + sc->nmenu; k++) {
        struct sc_rect e;
        bool ok;

        if (k < sc->nwin) {
            const struct sc_window *w = &sc->win[k];

            ok = live_strip(w) && live_rect(sc, w->x, w->y, w->w, w->h, &e);
        } else {
            const struct sc_menu *m = &sc->menu[k - sc->nwin];

            ok = (m->flags & (SC_GLASS | SC_LIVE)) == (SC_GLASS | SC_LIVE)
                 && live_rect(sc, m->x, m->y, m->w, m->h, &e);
        }

        for (i = 0; ok && i < n; i++) {
            struct sc_rect *a = &r[i];
            long x0, y0, x1, y1;

            if (a->w <= 0 || a->h <= 0 || a->x >= e.x + e.w || e.x >= a->x + a->w
                || a->y >= e.y + e.h || e.y >= a->y + a->h) {
                continue;
            }

            x0 = a->x < e.x ? a->x : e.x;
            y0 = a->y < e.y ? a->y : e.y;
            x1 = a->x + a->w > e.x + e.w ? a->x + a->w : e.x + e.w;
            y1 = a->y + a->h > e.y + e.h ? a->y + a->h : e.y + e.h;

            if (x0 != a->x || y0 != a->y || x1 - x0 != a->w || y1 - y0 != a->h) {
                *a = (struct sc_rect){ x0, y0, x1 - x0, y1 - y0 };
                grew++;
            }
        }
    }

    return grew;
}

/*
 * **The star specular** (Liquid Glass L5): where the window in front's rim
 * is lit most - its top-left corner, at the arc's 45 degrees - the light
 * ends in a small four-point star, Kosmos's mark made of the light itself.
 * Nine pixels across, its arms fading from the middle, toward the warm
 * light, as strong as the look's specular. Inside `x0`..`y1` only.
 */
static void star_glint(struct scene *sc, long cx, long cy, uint32_t toward,
                       long x0, long y0, long x1, long y1)
{
    static const unsigned arm[5] = { 255, 170, 100, 45, 15 };
    long i, k;

    for (k = 0; k < 4; k++) {
        for (i = k == 0 ? 0 : 1; i < 5; i++) {
            long x = cx + (k == 0 ? i : k == 1 ? -i : 0);
            long y = cy + (k == 2 ? i : k == 3 ? -i : 0);
            unsigned a = arm[i] * (unsigned)sc->rim_specular / 100u;
            uint32_t *o;

            if (x < x0 || x >= x1 || y < y0 || y >= y1 || a == 0) continue;
            o = row_of(sc->back, (unsigned)y) + x;
            *o = over((toward & 0x00ffffffu) | (uint32_t)a << 24, *o, 255);
        }
    }
}

/*
 * The rim's light, over a glass surface once its picture is blended: light
 * of colour `toward` added by the lens's specular where the picture draws,
 * on the side toward the light.
 */
static void rim_light(struct scene *sc, const struct surface *picture, long px, long py,
                      const struct rim_shape *shape, uint32_t toward,
                      long x0, long y0, long x1, long y1)
{
    const struct gfx_lens *l = shape != NULL ? shape->lens : NULL;
    long b, x, y, pw, ph;

    if (l == NULL) return;

    /* Only inside the shape: a strip's picture is larger than its glass. */
    if (x0 < shape->x) x0 = shape->x;
    if (y0 < shape->y) y0 = shape->y;
    if (x1 > shape->x + shape->w) x1 = shape->x + shape->w;
    if (y1 > shape->y + shape->h) y1 = shape->y + shape->h;
    pw = shape->w;
    ph = shape->h;
    b = (long)l->side;

    for (y = y0; y < y1; y++) {
        const uint32_t *p = row_of(picture, (unsigned)(y - py));
        uint32_t *o = row_of(sc->back, (unsigned)y);
        long ly = y - shape->y, by = ph - 1 - ly;
        bool band = ly < b || by < b;

        for (x = x0; x < x1; x++) {
            long lx = x - shape->x, rx = pw - 1 - lx;
            struct gfx_lens_px e;

            /* Away from every edge by more than a corner's square, nothing. */
            if (!band && lx >= b && rx >= b) {
                x = shape->x + pw - b - 1;
                continue;
            }

            if ((p[x - px] >> 24) == 0) continue;

            e = lens_at(l, lx, ly, pw, ph);

            if (e.spec != 0) o[x] = lit(o[x], e.spec, toward);
        }
    }
}

/*
 * **Starlight's horizon** (S3, `docs/astra.md`): the one-pixel line of the
 * look's colour along the top of a glass surface, over its picture once it
 * is blended - its shape's first row, wherever the picture draws there.
 * Solid, not by the picture's alpha: a glass surface's picture is its
 * light tint, and the line by that was a third of an amber (the first
 * gate). A cut or a rounded corner draws nothing at its ends, so the line
 * stops where the surface does; a menu's corners are put back after.
 */
static void horizon(struct scene *sc, const struct surface *picture, long px, long py,
                    const struct rim_shape *shape, long x0, long y0, long x1, long y1)
{
    uint32_t c = sc->rim_horizon;
    const uint32_t *p;
    uint32_t *o;
    long x, y;

    if (c == 0 || shape == NULL) return;

    y = shape->y;
    if (y < y0 || y >= y1 || y < py || y >= py + (long)picture->height) return;
    if (x0 < shape->x) x0 = shape->x;
    if (x1 > shape->x + shape->w) x1 = shape->x + shape->w;
    if (x0 < px) x0 = px;
    if (x1 > px + (long)picture->width) x1 = px + (long)picture->width;

    p = row_of(picture, (unsigned)(y - py));
    o = row_of(sc->back, (unsigned)y);

    for (x = x0; x < x1; x++) {
        if ((p[x - px] >> 24) != 0) o[x] = c | 0xff000000u;
    }
}

/*
 * The lens for the scene's corner and rim, made when one of the four
 * changed since it was last made - once a look, never in a frame. A rim
 * the lens cannot be made for - no bevel, a corner or a bevel past
 * `GFX_LENS_MOST`, no memory - is no rim, and the glass is flat.
 */
static void lens_update(struct scene *sc)
{
    long want[4] = { sc->corner, sc->rim_bevel, sc->rim_refraction, sc->rim_specular };
    size_t bytes;

    if (sc->lens_mem != NULL && memcmp(want, sc->lens_of, sizeof(want)) == 0) {
        return;
    }

    free(sc->lens_mem);
    sc->lens_mem = NULL;
    sc->lens_ok = false;
    memcpy(sc->lens_of, want, sizeof(want));

    if (sc->rim_bevel <= 0 || sc->corner < 0 || sc->rim_refraction < 0
        || sc->rim_specular < 0 || sc->rim_refraction > 100 || sc->rim_specular > 100) {
        return;
    }

    bytes = gfx_lens_size((unsigned)sc->corner, (unsigned)sc->rim_bevel);

    if (bytes == 0 || (sc->lens_mem = malloc(bytes)) == NULL) {
        return;
    }

    sc->lens_ok = gfx_lens_make(&sc->lens, sc->lens_mem, (unsigned)sc->corner,
                                (unsigned)sc->rim_bevel, (unsigned)sc->rim_refraction,
                                (unsigned)sc->rim_specular) == 1;
}

/*
 * **A lens for another radius** (LG4): a strip's glass, rounded at `radius`
 * rather than the scene's corner - the dock's, a banner's. The scene's own
 * when the radius is its corner; otherwise one of `rims`, made the first
 * time a radius is asked for under the look's bevel, refraction and
 * specular and kept, so a frame makes one only when a look or a radius is
 * new. NULL when none can be: no bevel, a radius past the lens's most, or
 * no memory - flat glass, as before.
 */
static const struct gfx_lens *lens_for(struct scene *sc, long radius)
{
    long want[4] = { radius, sc->rim_bevel, sc->rim_refraction, sc->rim_specular };
    unsigned n = sizeof sc->rims / sizeof sc->rims[0], i;
    size_t bytes;

    if (radius == sc->corner) return sc->lens_ok ? &sc->lens : NULL;

    for (i = 0; i < n; i++) {
        if (sc->rims[i].mem != NULL && memcmp(want, sc->rims[i].of, sizeof(want)) == 0) {
            return sc->rims[i].ok ? &sc->rims[i].lens : NULL;
        }
    }

    if (radius <= 0 || sc->rim_bevel <= 0 || sc->rim_refraction < 0 || sc->rim_specular < 0
        || sc->rim_refraction > 100 || sc->rim_specular > 100) {
        return NULL;
    }

    bytes = gfx_lens_size((unsigned)radius, (unsigned)sc->rim_bevel);
    i = sc->rims_next;
    sc->rims_next = (i + 1) % n;
    free(sc->rims[i].mem);
    sc->rims[i].mem = bytes != 0 ? malloc(bytes) : NULL;
    sc->rims[i].ok = false;
    memcpy(sc->rims[i].of, want, sizeof(want));

    if (sc->rims[i].mem == NULL) return NULL;

    sc->rims[i].ok = gfx_lens_make(&sc->rims[i].lens, sc->rims[i].mem, (unsigned)radius,
                                   (unsigned)sc->rim_bevel, (unsigned)sc->rim_refraction,
                                   (unsigned)sc->rim_specular) == 1;
    return sc->rims[i].ok ? &sc->rims[i].lens : NULL;
}

/* A strip's glass shape (`sc_window.rr`), on the screen, with its lens: true
 * when it has one that fits inside its picture and can be bent. */
static bool strip_shape(struct scene *sc, const struct sc_window *w, struct rim_shape *out)
{
    const struct gfx_lens *l;

    if (w->rr < 0 || w->rx < 0 || w->ry < 0 || w->rw <= 0 || w->rh <= 0
        || w->rx + w->rw > w->w || w->ry + w->rh > w->h) {
        return false;
    }

    /* No lens - a look with no rim, a square strip - is a shape still,
     * flat: what the horizon is drawn along. */
    l = w->rr > 0 ? lens_for(sc, w->rr) : NULL;

    if (l != NULL && (w->rw < 2 * (long)l->side || w->rh < 2 * (long)l->side)) l = NULL;

    *out = (struct rim_shape){ l, w->x + w->rx, w->y + w->ry, w->rw, w->rh };
    return true;
}

static void draw_window(struct scene *sc, const struct sc_window *w, const struct sc_rect *r)
{
    long kept[4][6];
    int nkept = 0;
    long x0, y0, x1, y1;
    bool starred = false, shaped = false;
    struct rim_shape shape;

    if ((w->flags & SC_HIDDEN) || w->x >= r->x + r->w || w->x + w->w <= r->x
        || w->y >= r->y + r->h || w->y + w->h <= r->y) {
        return;
    }

    if (w->flags & SC_ROUNDED) {
        nkept = corners(sc, w->x, w->y, w->w, w->h, r, kept, sc->corner);
        keep_corners(sc, kept, nkept);
    }

    x0 = w->x > r->x ? w->x : r->x;
    y0 = w->y > r->y ? w->y : r->y;
    x1 = w->x + w->w < r->x + r->w ? w->x + w->w : r->x + r->w;
    y1 = w->y + w->h < r->y + r->h ? w->y + w->h : r->y + r->h;

    if (x1 > x0 && y1 > y0 && w->picture != NULL) {
        if (w->flags & SC_BLEND) {
            if ((w->flags & SC_GLASS) && sc->glass != NULL && sc->glass->pixels != NULL
                && w->src_w == w->w && w->src_h == w->h) {
                struct glass_was was;
                bool live = live_under(sc, w->flags, w->x, w->y, w->w, w->h, &was);

                shaped = strip_shape(sc, w, &shape);
                glass_under(sc, w->picture, w->x, w->y, shaped ? &shape : NULL,
                            x0, y0, x1, y1);
                if (live) live_done(sc, &was);
            }
            blend_on(sc->back, w->picture, x0 - w->x, y0 - w->y, x1 - x0, y1 - y0,
                     x0, y0, 255);
            /* And its rim's light, cool, over the picture (LG4). */
            if (shaped) {
                rim_light(sc, w->picture, w->x, w->y, &shape, 0xffffffffu, x0, y0, x1, y1);
                horizon(sc, w->picture, w->x, w->y, &shape, x0, y0, x1, y1);
            }
        } else if ((w->flags & SC_GLASS) && sc->glass != NULL && sc->glass->pixels != NULL
                   && w->bar == NULL && !(w->flags & SC_STALE)
                   && w->src_w == w->w && w->src_h == w->h) {
            /* A window whose ground is glass (LG4): glass without
             * `SC_BLEND`, frosted only when its picture is shown as it is;
             * stretched in a resize, stale or under a menu bar it is
             * copied below as any window is. Its inset still goes over. */
            shape = (struct rim_shape){ sc->lens_ok && (w->flags & SC_ROUNDED) ? &sc->lens : NULL,
                                        w->x, w->y, w->w, w->h };
            glass_under(sc, w->picture, w->x, w->y, &shape, x0, y0, x1, y1);
            blend_on(sc->back, w->picture, x0 - w->x, y0 - w->y, x1 - x0, y1 - y0,
                     x0, y0, 255);
            horizon(sc, w->picture, w->x, w->y, &shape, x0, y0, x1, y1);
            /* Warm on the window in front, cool on the rest (L6): focus
             * shown by the light in the glass. */
            if (w->flags & SC_ROUNDED) {
                rim_light(sc, w->picture, w->x, w->y, &shape,
                          (w->flags & SC_FOCUSED) && sc->rim_front != 0
                              ? sc->rim_front : 0xffffffffu,
                          x0, y0, x1, y1);

                /* And its star, once everything over the header is on. */
                starred = (w->flags & SC_FOCUSED) && sc->lens_ok && sc->rim_specular > 0;
            }
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
                    stale_into(sc, w, split, lower, -1, x0, ya, x1, y1);
                } else if (w->src_w != w->w || w->src_h != lower) {
                    stretch_into(sc->back, w->picture, 0, 0, w->src_w, w->src_h,
                                 w->x, split, w->w, lower, -1, false,
                                 x0, ya, x1, y1);
                } else {
                    blit_on(sc->back, w->picture, x0 - w->x, ya - split, x1 - x0, y1 - ya,
                            x0, ya);
                }
            }
        } else if ((w->flags & SC_GLASS) && sc->glass != NULL && sc->glass->pixels != NULL) {
            /*
             * A glass window in a resize (LG4): stretched or stale, its
             * picture no longer lies over its frame pixel for pixel, so the
             * wallpaper's glass is laid over the whole frame, flat, and the
             * picture blended over that as it is stretched or drawn out -
             * glass all the way, rather than its tint as a solid colour
             * until the resize ends. Its rim comes back with its picture.
             */
            glass_flat(sc, x0, y0, x1, y1);

            if (w->flags & SC_STALE) {
                stale_into(sc, w, w->y, w->h, 255, x0, y0, x1, y1);
            } else {
                stretch_into(sc->back, w->picture, 0, 0, w->src_w, w->src_h,
                             w->x, w->y, w->w, w->h, 255, false, x0, y0, x1, y1);
            }
        } else if (w->flags & SC_STALE) {
            stale_into(sc, w, w->y, w->h, -1, x0, y0, x1, y1);
        } else if (w->src_w != w->w || w->src_h != w->h) {
            stretch_into(sc->back, w->picture, 0, 0, w->src_w, w->src_h,
                         w->x, w->y, w->w, w->h, -1, false, x0, y0, x1, y1);
        } else {
            blit_on(sc->back, w->picture, x0 - w->x, y0 - w->y, x1 - x0, y1 - y0, x0, y0);
        }

        /* And its inset over it, below a menu bar when there is one. */
        if (!(w->flags & SC_BLEND)) {
            long top = w->bar != NULL ? w->y + w->bar_h : w->y;

            inset_into(sc, w, top, w->y + w->h - top, x0, y0, x1, y1);
        }
    }

    /* Its plate over its header (Starlight's), blended for the slant. */
    if (w->plate != NULL && x1 > x0 && y1 > y0) {
        long dx = w->x + w->px, dy = w->y + w->py;
        long ax0 = x0 > dx ? x0 : dx;
        long ay0 = y0 > dy ? y0 : dy;
        long ax1 = x1 < dx + (long)w->plate->width ? x1 : dx + (long)w->plate->width;
        long ay1 = y1 < dy + (long)w->plate->height ? y1 : dy + (long)w->plate->height;

        if (ax1 > ax0 && ay1 > ay0) {
            blend_on(sc->back, w->plate, ax0 - dx, ay0 - dy, ax1 - ax0, ay1 - ay0,
                     ax0, ay0, 255);
        }
    }

    /* The star specular, last, so that a plate over the corner does not
     * hide it: the light's end is over everything the header holds. */
    if (starred) {
        long in = (long)sc->corner * 29 / 100;

        star_glint(sc, w->x + in, w->y + in,
                   sc->rim_front != 0 ? sc->rim_front : 0xffffffffu, x0, y0, x1, y1);
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
    if ((w->flags & SC_LIGHTS) && w->by < r->y + r->h && w->by + sc->box > r->y) {
        int slot;

        for (slot = 0; slot < SC_SLOTS; slot++) {
            long x = box_left(sc, w, slot);
            bool held = w->held == slot;
            bool off = !(w->flags & SC_FOCUSED) || (slot == 0 && !(w->flags & SC_MAXIMISE));

            if (x >= r->x + r->w || x + sc->box <= r->x) {
                continue;
            }

            if (!(w->flags & SC_PLATINUM)) {
                light(sc, x, w->by, slot, (w->flags & SC_LIT) || held, off, held);
            } else if (w->flags & SC_FOCUSED) {
                /* Zoom on a window that cannot be zoomed is not there. */
                if (slot != 0 || (w->flags & SC_MAXIMISE)) {
                    platinum_box(sc, x, w->by, slot, held);
                }
            }
        }
    }

    if (nkept > 0) {
        put_back(sc, kept, nkept, w->x, w->y, w->w, w->h, sc->corner, 0);
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

    lens_update(sc);
    sc->cur = r;
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
        long kept[4][6], size;
        int nkept;

        if (x1 <= x0 || y1 <= y0 || m->picture == NULL) {
            continue;
        }

        /* A menu under a look with the cut keeps a square large enough
         * for it at each corner (`sc->cut`, Starlight's). */
        size = sc->cut > sc->corner ? sc->cut : sc->corner;
        nkept = corners(sc, m->x, m->y, m->w, m->h, &r, kept, size);
        keep_corners(sc, kept, nkept);

        /* Under a glass look, a menu is glass as the dock is (LG4): the
         * wallpaper's glass where it draws, its tint blended over that. */
        if ((m->flags & SC_GLASS) && sc->glass != NULL && sc->glass->pixels != NULL
            && m->w <= (long)m->picture->width && m->h <= (long)m->picture->height) {
            struct glass_was was;
            bool live = live_under(sc, m->flags, m->x, m->y, m->w, m->h, &was);

            struct rim_shape shape = { sc->lens_ok ? &sc->lens : NULL, m->x, m->y, m->w, m->h };

            glass_under(sc, m->picture, m->x, m->y, &shape, x0, y0, x1, y1);
            if (live) live_done(sc, &was);
            blend_on(sc->back, m->picture, x0 - m->x, y0 - m->y, x1 - x0, y1 - y0,
                     x0, y0, 255);
            rim_light(sc, m->picture, m->x, m->y, &shape, 0xffffffffu, x0, y0, x1, y1);
            horizon(sc, m->picture, m->x, m->y, &shape, x0, y0, x1, y1);
        } else {
            blit_on(sc->back, m->picture, x0 - m->x, y0 - m->y, x1 - x0, y1 - y0, x0, y0);
        }
        put_back(sc, kept, nkept, m->x, m->y, m->w, m->h, size, sc->cut);
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
