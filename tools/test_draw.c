/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Graphics Kit's primitives and the scene, on this Mac (Astra's D3b-1,
 * `docs/astra-display.md`).
 *
 *   build/host/test_draw
 *
 * `draw.c` and `scene_core.c` need no Lua since D3b-1 - the kit binds them
 * and the window server will hold them - so they are tested here, as C:
 * a fill and a copy clipped at the edges, a blend at half strength, a
 * stretch doubled, a rounded corner put back, a triangle's rows; and a
 * scene of two windows over each other and a menu over both, composed:
 * the desktop where neither is, the upper window where they overlap, the
 * menu over everything, a hidden window not drawn, a rounded corner
 * showing what is under it, a drag's label drawn last and only inside the
 * rectangle composed, and a rectangle composed again giving the same
 * pixels.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "surface.h"
#include "draw_primitives.h"
#include "scene_core.h"
#include "blur.h"

static int failed, passed;

static void check(bool ok, const char *what)
{
    if (ok) {
        passed++;
    } else {
        failed++;
        printf("  FAIL: %s\n", what);
    }
}

static struct surface make(unsigned w, unsigned h, uint32_t colour)
{
    struct surface s;
    unsigned x, y;

    s.width = w;
    s.height = h;
    s.pitch = w * 4;
    s.pixels = malloc((size_t)w * h * 4);
    s.pages = 0;
    s.owned = false;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            s.pixels[y * w + x] = colour;
        }
    }

    return s;
}

static uint32_t at(const struct surface *s, unsigned x, unsigned y)
{
    return row_of(s, y)[x];
}

static void primitives(void)
{
    struct surface a = make(16, 16, 0xff000000u);
    struct surface b = make(8, 8, 0xff00ff00u);
    struct surface half = make(4, 4, 0x80ffffffu);
    struct surface big = make(8, 8, 0xff000000u);
    struct surface two = make(2, 2, 0xff112233u);
    double vx[3] = { 0.0, 16.0, 0.0 }, vy[3] = { 0.0, 0.0, 16.0 };
    unsigned n = 0, x, y;

    fill_on(&a, -4, -4, 8, 8, 0xffff0000u);
    check(at(&a, 3, 3) == 0xffff0000u && at(&a, 4, 4) == 0xff000000u,
          "a fill off the top-left edge clipped to what is on the surface");

    blit_on(&a, &b, 0, 0, 8, 8, 12, 12);
    check(at(&a, 15, 15) == 0xff00ff00u && at(&a, 11, 11) == 0xff000000u,
          "a copy off the bottom-right edge clipped");

    blend_on(&a, &half, 0, 0, 4, 4, 6, 6, 255);
    check((at(&a, 6, 6) & 0xffu) >= 0x7e && (at(&a, 6, 6) & 0xffu) <= 0x81,
          "white at half alpha over black is half grey");

    stretch_into(&big, &two, 0, 0, 2, 2, 0, 0, 8, 8, -1, false, 0, 0, 8, 8);
    check(at(&big, 7, 7) == 0xff112233u && at(&big, 0, 0) == 0xff112233u,
          "two pixels stretched to eight cover them");

    fill_on(&a, 0, 0, 16, 16, 0xff000000u);
    triangle_on(&a, vx, vy, 0xffffffffu);

    for (y = 0; y < 16; y++) {
        for (x = 0; x < 16; x++) {
            n += at(&a, x, y) == 0xffffffffu;
        }
    }

    check(n >= 120 && n <= 136, "a right triangle of side 16 fills half of 256");

    free(a.pixels);
    free(b.pixels);
    free(half.pixels);
    free(big.pixels);
    free(two.pixels);
}

static void scene(void)
{
    struct surface back = make(200, 120, 0xff000000u);
    struct surface w1 = make(100, 60, 0xff0000ffu);      /* blue, behind */
    struct surface w2 = make(100, 60, 0xffff0000u);      /* red, in front */
    struct surface menu = make(40, 30, 0xff00ff00u);     /* green, over all */
    struct surface badge = make(12, 8, 0xffffff00u);     /* yellow, a label */
    struct surface panel = make(8, 40, 0x80ffffffu);     /* half white, a panel */
    struct surface ref = make(200, 120, 0);
    struct scene *sc = malloc(sizeof(*sc));
    long drawn;
    unsigned x, y, same = 1;

    scene_init(sc, &back);
    sc->desktop = 0xff808080u;
    sc->corner = 8;
    check(scene_set_windows(sc, 3) && scene_set_menus(sc, 1), "room for three windows and a menu");

    sc->win[0] = (struct sc_window){ 10, 10, 100, 60, SC_ROUNDED, &w1, 100, 60, NULL, 0, 0, 0, -1, 0, NULL, 0, 0, 0, 0, NULL, 0, 0, -1, -1, -1, -1 };
    sc->win[1] = (struct sc_window){ 60, 40, 100, 60, SC_ROUNDED, &w2, 100, 60, NULL, 0, 0, 0, -1, 0, NULL, 0, 0, 0, 0, NULL, 0, 0, -1, -1, -1, -1 };
    sc->win[2] = (struct sc_window){ 150, 5, 40, 40, SC_HIDDEN, &w1, 100, 60, NULL, 0, 0, 0, -1, 0, NULL, 0, 0, 0, 0, NULL, 0, 0, -1, -1, -1, -1 };
    sc->menu[0] = (struct sc_menu){ 140, 80, 40, 30, &menu, -1, 0 };

    drawn = scene_compose(sc, 0, 0, 200, 120);
    check(drawn > 0, "the scene composed");
    check(at(&back, 2, 2) == 0xff808080u, "the desktop where no window is");
    check(at(&back, 20, 20) == 0xff0000ffu, "the window behind where only it is");
    check(at(&back, 70, 50) == 0xffff0000u, "the window in front where they overlap");
    check(at(&back, 150, 90) == 0xff00ff00u, "the menu over the window in front");
    check(at(&back, 170, 20) == 0xff808080u, "a hidden window not drawn");
    check(at(&back, 60, 40) != 0xffff0000u,
          "the front window's corner rounded, showing what is under it");

    /* A drag's label, last of all, and only inside the rectangle composed. */
    sc->badge = &badge;
    sc->badge_x = 4;
    sc->badge_y = 100;
    (void)scene_compose(sc, 0, 0, 8, 120);
    check(at(&back, 6, 104) == 0xffffff00u, "a drag's label over the desktop");
    check(at(&back, 10, 104) == 0xff808080u, "and not past the rectangle composed");
    (void)scene_compose(sc, 0, 0, 200, 120);
    sc->badge = NULL;

    /* A panel over everything, by its own alpha: half white over the
     * desktop's grey is a lighter grey, and nothing where it is clear. */
    for (unsigned yy = 20; yy < 40; yy++) {
        for (unsigned xx = 0; xx < 8; xx++) {
            row_of(&panel, yy)[xx] = 0;
        }
    }

    sc->panel = &panel;
    sc->panel_x = 0;
    sc->panel_y = 0;
    (void)scene_compose(sc, 0, 0, 200, 120);
    check((at(&back, 4, 4) & 0xffu) > 0xb0 && (at(&back, 4, 4) & 0xffu) < 0xd0,
          "a panel blended by its own alpha over the desktop");
    check(at(&back, 4, 30) == 0xff808080u, "and the desktop where the panel is clear");
    sc->panel = NULL;
    (void)scene_compose(sc, 0, 0, 200, 120);

    memcpy(ref.pixels, back.pixels, (size_t)200 * 120 * 4);
    fill_on(&back, 0, 0, 200, 120, 0xffff00ffu);
    (void)scene_compose(sc, 0, 0, 200, 120);

    for (y = 0; y < 120; y++) {
        for (x = 0; x < 200; x++) {
            same &= at(&back, x, y) == at(&ref, x, y);
        }
    }

    check(same, "composed again over another colour, the same pixels");

    scene_free(sc);
    free(sc);
    free(back.pixels);
    free(w1.pixels);
    free(w2.pixels);
    free(menu.pixels);
    free(badge.pixels);
    free(panel.pixels);
    free(ref.pixels);
}

/*
 * **A glass strip** (`SC_GLASS`, Liquid Glass LG4): a blended strip over a
 * window, with the wallpaper's glass somewhere else entirely. Where the
 * strip draws its tint, the tint is over the glass and not over the window
 * under it; where it draws nothing, the window shows; past the glass's
 * edge it is over the desktop's colour. Without the flag, the same strip
 * is over the window - the control, in the same scene.
 */
static void glass(void)
{
    struct surface back = make(120, 60, 0xff000000u);
    struct surface win = make(120, 60, 0xff0000ffu);      /* blue, under it */
    struct surface strip = make(60, 20, 0x80ffffffu);     /* half white */
    struct surface frost = make(100, 60, 0xff00ff00u);    /* green: the glass */
    struct scene *sc = malloc(sizeof(*sc));
    unsigned x, y;
    uint32_t p;

    /* The strip's left quarter draws nothing: a pill's rounded end. */
    for (y = 0; y < 20; y++) {
        for (x = 0; x < 15; x++) row_of(&strip, y)[x] = 0;
    }

    scene_init(sc, &back);
    sc->desktop = 0xffff0000u;                             /* red past it */
    sc->glass = &frost;
    sc->wx = sc->wy = 0;
    check(scene_set_windows(sc, 2), "room for a window and a strip");
    sc->win[0] = (struct sc_window){ 0, 0, 120, 60, 0, &win, 120, 60, NULL, 0, 0, 0, -1, 0, NULL, 0, 0, 0, 0, NULL, 0, 0, -1, -1, -1, -1 };
    sc->win[1] = (struct sc_window){ 50, 20, 60, 20, SC_BLEND | SC_GLASS, &strip, 60, 20, NULL, 0, 0, 0, -1, 0, NULL, 0, 0, 0, 0, NULL, 0, 0, -1, -1, -1, -1 };

    (void)scene_compose(sc, 0, 0, 120, 60);
    p = at(&back, 80, 30);
    check(((p >> 8) & 0xff) > 0xb0 && (p & 0xff) < 0x90 && (p & 0xff) > 0x70,
          "a glass strip's tint over the glass, not over the window under it");
    check(at(&back, 55, 30) == 0xff0000ffu, "where the strip draws nothing, the window under it");
    p = at(&back, 105, 30);
    check(((p >> 16) & 0xff) > 0xb0 && ((p >> 8) & 0xff) > 0x70 && ((p >> 8) & 0xff) < 0x90,
          "past the glass's edge, the tint over the desktop's colour");

    sc->win[1].flags = SC_BLEND;
    (void)scene_compose(sc, 0, 0, 120, 60);
    p = at(&back, 80, 30);
    check(((p >> 8) & 0xff) < 0x90 && (p & 0xff) > 0xb0,
          "the same strip, not glass, over the window under it");

    /* A menu, on the scene's own list, glass the same way: the strip as a
     * menu's picture, where the strip was, and the strip itself gone. */
    check(scene_set_windows(sc, 1) && scene_set_menus(sc, 1), "room for a window and a menu");
    sc->menu[0] = (struct sc_menu){ 50, 20, 60, 20, &strip, -1, SC_GLASS };
    (void)scene_compose(sc, 0, 0, 120, 60);
    p = at(&back, 80, 30);
    check(((p >> 8) & 0xff) > 0xb0 && (p & 0xff) < 0x90 && (p & 0xff) > 0x70,
          "a glass menu's tint over the glass, not over the window under it");
    check(at(&back, 55, 30) == 0xff0000ffu, "where a glass menu draws nothing, the window under it");

    /* And without the flag, copied as a menu always was: its pixels as
     * they are, alpha and all, whatever is under it. */
    sc->menu[0].flags = 0;
    (void)scene_compose(sc, 0, 0, 120, 60);
    check(at(&back, 80, 30) == 0x80ffffffu, "a menu that is not glass, copied as it is");

    /* A window whose ground is glass: `SC_GLASS` without `SC_BLEND`, the
     * strip as its picture over the blue window. Frosted when shown as it
     * is; stretched, as in a resize, copied as any window is. */
    check(scene_set_windows(sc, 2) && scene_set_menus(sc, 0), "room for two windows");
    sc->win[1] = (struct sc_window){ 50, 20, 60, 20, SC_GLASS, &strip, 60, 20, NULL, 0, 0, 0, -1, 0, NULL, 0, 0, 0, 0, NULL, 0, 0, -1, -1, -1, -1 };
    (void)scene_compose(sc, 0, 0, 120, 60);
    p = at(&back, 80, 30);
    check(((p >> 8) & 0xff) > 0xb0 && (p & 0xff) < 0x90 && (p & 0xff) > 0x70,
          "a glass window's ground over the glass, not over the window under it");
    sc->win[1].src_w = 50;
    (void)scene_compose(sc, 0, 0, 120, 60);
    check(at(&back, 80, 30) == 0x80ffffffu, "a glass window stretched in a resize, copied");

    scene_free(sc);
    free(sc);
    free(back.pixels);
    free(win.pixels);
    free(strip.pixels);
    free(frost.pixels);
}

/*
 * **Live glass** (Liquid Glass LG6): a glass strip over a window of
 * stripes four pixels wide, blue and yellow, with the wallpaper's glass
 * green somewhere else. The strip's picture is black at an alpha of 1, so
 * what shows under it is its glass within a step. Live, at a radius of 3:
 * every pixel of the strip is the stripes blurred - held to `gfx_blur`
 * over the same back buffer composed without the strip, its reach and
 * all - and neither green nor the sharp stripes. A rectangle that does not
 * hold the reach is wallpaper glass, as before, and `scene_widen` is what
 * makes one hold it. Clear glass is the stripes, sharp; none is green. A
 * glass menu is live the same way.
 */
static bool near(uint32_t a, uint32_t b, unsigned by)
{
    unsigned k;

    for (k = 0; k < 24; k += 8) {
        int d = (int)((a >> k) & 0xffu) - (int)((b >> k) & 0xffu);

        if (d > (int)by || d < -(int)by) return false;
    }
    return true;
}

static void live(void)
{
    enum { W = 160, H = 80, SX = 50, SY = 30, SW = 60, SH = 20, R = 3 };
    struct surface back = make(W, H, 0xff000000u);
    struct surface win = make(W, H, 0);
    struct surface strip = make(SW, SH, 0x01000000u);
    struct surface frost = make(W, H, 0xff00ff00u);
    struct scene *sc = malloc(sizeof(*sc));
    long reach = 3 * R, ex = SX - reach, ey = SY - reach;
    long ew = SW + 2 * reach, eh = SH + 2 * reach;
    uint32_t *ref = malloc((size_t)ew * (size_t)eh * 4);
    uint32_t *tmp = malloc((size_t)ew * (size_t)eh * 4);
    uint32_t *sums = malloc((size_t)ew * 16);
    struct sc_rect r[2];
    unsigned x, y, off = 0, green = 0, sharp = 0, clear_off = 0;
    long y2;

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) row_of(&win, y)[x] = (x / 4) % 2 ? 0xffffff00u : 0xff0000ffu;
    }

    scene_init(sc, &back);
    sc->desktop = 0xffff0000u;
    sc->glass = &frost;
    sc->wx = sc->wy = 0;
    check(scene_set_windows(sc, 1), "room for the window under live glass");
    sc->win[0] = (struct sc_window){ 0, 0, W, H, 0, &win, W, H, NULL, 0, 0, 0, -1, 0, NULL, 0, 0, 0, 0, NULL, 0, 0, -1, -1, -1, -1 };

    /* The reference: the back buffer without the strip, its reach blurred. */
    (void)scene_compose(sc, 0, 0, W, H);
    for (y2 = 0; y2 < eh; y2++) {
        memcpy(ref + y2 * ew, row_of(&back, (unsigned)(ey + y2)) + ex, (size_t)ew * 4);
    }
    gfx_blur(ref, ref, (size_t)ew, (unsigned)ew, (unsigned)eh, R, 3, tmp, sums);

    check(scene_set_windows(sc, 2), "room for the window and a strip");
    sc->win[1] = (struct sc_window){ SX, SY, SW, SH, SC_BLEND | SC_GLASS | SC_LIVE, &strip, SW, SH, NULL, 0, 0, 0, -1, 0, NULL, 0, 0, 0, 0, NULL, 0, 0, -1, -1, -1, -1 };
    sc->live_blur = R;
    sc->live_made = 0;
    (void)scene_compose(sc, 0, 0, W, H);

    for (y = SY; y < SY + SH; y++) {
        for (x = SX; x < SX + SW; x++) {
            uint32_t p = at(&back, x, y), q = ref[(y - ey) * ew + (x - ex)];

            if (!near(p, q, 2)) off++;
            if (near(p, 0xff00ff00u, 8)) green++;
            if (near(p, at(&win, x, y), 8)) sharp++;
        }
    }
    check(sc->live_made == 1, "one surface made live in one rectangle");
    check(off == 0, "live glass: the strip is what is under it blurred, as gfx_blur blurs it");
    check(green == 0, "live glass: not the wallpaper's glass");
    check(sharp < SW * SH / 10, "live glass: blurred, not the stripes as they are");

    /* A rectangle that does not hold the reach: wallpaper glass, as before. */
    sc->live_made = 0;
    (void)scene_compose(sc, SX, SY, SW, SH);
    check(sc->live_made == 0 && near(at(&back, SX + 30, SY + 10), 0xff00ff00u, 2),
          "a rectangle short of the reach: wallpaper glass, not a blur of a frame before");

    /* `scene_widen`: a rectangle touching the reach grows to hold it; one
     * away from it does not; and a second asking grows nothing. */
    r[0] = (struct sc_rect){ SX + 10, SY + 5, 4, 4 };
    r[1] = (struct sc_rect){ 0, 0, 10, 10 };
    check(scene_widen(sc, r, 2) == 1 && r[0].x == ex && r[0].y == ey && r[0].w == ew
          && r[0].h == eh && r[1].w == 10, "the damage widened to the strip's reach, and only there");
    check(scene_widen(sc, r, 2) == 0, "widened once, nothing more to widen");

    /* As the window manager's own scene holds it: no pictures, no glass -
     * slots in the window server - and widened all the same. */
    {
        struct surface *pic = sc->win[1].picture, *g = sc->glass;

        sc->win[1].picture = NULL;
        sc->glass = NULL;
        r[0] = (struct sc_rect){ SX + 10, SY + 5, 4, 4 };
        check(scene_widen(sc, r, 1) == 1 && r[0].w == ew,
              "widened in a scene that names its pictures by slot");
        sc->win[1].picture = pic;
        sc->glass = g;
    }

    /* Glass but not live (`SC_LIVE` absent: the desktop's own window):
     * wallpaper glass, and no damage widened for it. */
    sc->win[1].flags = SC_BLEND | SC_GLASS;
    sc->live_made = 0;
    (void)scene_compose(sc, 0, 0, W, H);
    r[0] = (struct sc_rect){ SX + 10, SY + 5, 4, 4 };
    check(sc->live_made == 0 && near(at(&back, SX + 30, SY + 10), 0xff00ff00u, 2)
          && scene_widen(sc, r, 1) == 0, "glass that is not live: the wallpaper's glass, nothing widened");
    sc->win[1].flags = SC_BLEND | SC_GLASS | SC_LIVE;
    sc->live_blur = -1;
    r[0] = (struct sc_rect){ SX + 10, SY + 5, 4, 4 };
    check(scene_widen(sc, r, 1) == 0 && r[0].w == 4, "no live glass, no damage widened");

    /* Clear glass: the stripes as they are; none: the wallpaper's glass. */
    sc->live_blur = 0;
    (void)scene_compose(sc, 0, 0, W, H);
    for (y = SY; y < SY + SH; y++) {
        for (x = SX; x < SX + SW; x++) {
            if (!near(at(&back, x, y), at(&win, x, y), 1)) clear_off++;
        }
    }
    check(clear_off == 0, "clear live glass: what is under it, sharp");
    sc->live_blur = -1;
    (void)scene_compose(sc, 0, 0, W, H);
    check(near(at(&back, SX + 30, SY + 10), 0xff00ff00u, 2), "no live glass: the wallpaper's glass");

    /* A glass menu, live the same way. */
    check(scene_set_windows(sc, 1) && scene_set_menus(sc, 1), "room for the window and a menu");
    sc->menu[0] = (struct sc_menu){ SX, SY, SW, SH, &strip, -1, SC_GLASS | SC_LIVE };
    sc->live_blur = R;
    off = 0;
    (void)scene_compose(sc, 0, 0, W, H);
    for (y = SY; y < SY + SH; y++) {
        for (x = SX; x < SX + SW; x++) {
            if (!near(at(&back, x, y), ref[(y - ey) * ew + (x - ex)], 2)) off++;
        }
    }
    check(off == 0, "a live glass menu: what is under it blurred");

    scene_free(sc);
    free(sc);
    free(ref);
    free(tmp);
    free(sums);
    free(back.pixels);
    free(win.pixels);
    free(strip.pixels);
    free(frost.pixels);
}

/*
 * **The rim** (Liquid Glass LG3, LG4): a glass window, rounded at 8, over
 * glass whose red is its x and green its y, so where a pixel's glass was
 * read from shows in it. Its picture is black at an alpha of 1, so what
 * shows is the glass within a step. With a bevel of 4 and no light: the
 * middle unbent, the left edge read from further in (more red), the top
 * from further down (more green). With light from the upper left: the left
 * edge brighter, the right edge not. And without a rim, the edge unbent.
 */
/*
 * **The beads** (Liquid Glass L5): under a rim a round button is a bead -
 * its upper left caught by the light, brighter than its lower right. With
 * no rim, the two are one colour.
 */
static void beads(void)
{
    struct surface back = make(120, 60, 0xff000000u);
    struct surface pic = make(100, 40, 0xff202020u);
    struct scene *sc = malloc(sizeof(*sc));
    uint32_t p, q;
    int s;

    scene_init(sc, &back);
    sc->desktop = 0xff000000u;
    sc->d = 14;
    sc->box = 16;
    sc->box_w = 20;
    for (s = 0; s < SC_SLOTS; s++) {
        sc->fill[s] = sc->held_fill[s] = 0xff803020u;
        sc->ring[s] = 0xff803020u;
    }
    check(scene_set_windows(sc, 1) && scene_set_menus(sc, 0), "room for a window with buttons");
    sc->win[0] = (struct sc_window){ 10, 10, 100, 40, SC_LIGHTS | SC_FOCUSED | SC_MAXIMISE, &pic, 100, 40, NULL, 0, 20, 12, 0, -1, NULL, 0, 0, 0, 0, NULL, 0, 0, -1, -1, -1, -1 };

    sc->rim_bevel = 4;
    (void)scene_compose(sc, 0, 0, 120, 60);
    p = at(&back, 20 + 1 + 5, 12 + 1 + 3);      /* a bead's upper left */
    q = at(&back, 20 + 1 + 10, 12 + 1 + 11);    /* and its lower right */
    check(((p >> 8) & 0xff) > ((q >> 8) & 0xff) + 30,
          "under a rim, a button's upper left lit like a bead");

    sc->rim_bevel = 0;
    (void)scene_compose(sc, 0, 0, 120, 60);
    p = at(&back, 20 + 1 + 5, 12 + 1 + 3);
    q = at(&back, 20 + 1 + 10, 12 + 1 + 11);
    check(p == q, "with no rim, a button one colour");

    scene_free(sc);
    free(sc);
    free(back.pixels);
    free(pic.pixels);
}

static void rim(void)
{
    struct surface back = make(120, 80, 0xff000000u);
    struct surface plate = make(40, 12, 0xff806020u);
    struct surface pic = make(80, 40, 0x01000000u);
    struct surface frost = make(120, 80, 0);
    struct scene *sc = malloc(sizeof(*sc));
    unsigned x, y;
    uint32_t p, q;

    for (y = 0; y < 80; y++) {
        for (x = 0; x < 120; x++) row_of(&frost, y)[x] = 0xff000040u | x << 16 | y << 8;
    }

    scene_init(sc, &back);
    sc->desktop = 0xff000000u;
    sc->glass = &frost;
    sc->corner = 8;
    sc->rim_bevel = 4;
    sc->rim_refraction = 60;
    sc->rim_specular = 0;
    check(scene_set_windows(sc, 1) && scene_set_menus(sc, 0), "room for a window");
    sc->win[0] = (struct sc_window){ 20, 20, 80, 40, SC_GLASS | SC_ROUNDED, &pic, 80, 40, NULL, 0, 0, 0, -1, 0, NULL, 0, 0, 0, 0, NULL, 0, 0, -1, -1, -1, -1 };

    (void)scene_compose(sc, 0, 0, 120, 80);
    check(sc->lens_ok, "the lens was made for a corner of 8 and a bevel of 4");
    p = at(&back, 60, 40);
    check(((p >> 16) & 0xff) + 1 >= 60 && ((p >> 16) & 0xff) <= 60
          && ((p >> 8) & 0xff) + 1 >= 40 && ((p >> 8) & 0xff) <= 40,
          "the middle of a glass window, its own glass unbent");
    p = at(&back, 20, 40);
    check(((p >> 16) & 0xff) > 21 && ((p >> 8) & 0xff) + 1 >= 40 && ((p >> 8) & 0xff) <= 40,
          "the left edge's glass read from further in, at its own height");
    p = at(&back, 60, 20);
    check(((p >> 8) & 0xff) > 21 && ((p >> 16) & 0xff) + 1 >= 60 && ((p >> 16) & 0xff) <= 60,
          "the top edge's glass read from further down, at its own column");

    sc->rim_specular = 85;
    (void)scene_compose(sc, 0, 0, 120, 80);
    p = at(&back, 20, 40);
    q = at(&back, 99, 40);
    check((p & 0xff) > 0x40 + 20, "the left edge, toward the light, lit");
    check((q & 0xff) <= 0x40, "the right edge, away from the light, not lit");

    /* In front, its light the look's warm one - orange here, so it shows:
     * the left edge's red brighter, its blue not. And behind, white. */
    sc->rim_front = 0xffff8000u;
    sc->win[0].flags |= SC_FOCUSED;
    (void)scene_compose(sc, 0, 0, 120, 80);
    p = at(&back, 20, 40);
    check(((p >> 16) & 0xff) > 40 && (p & 0xff) <= 0x40,
          "the window in front's rim lit warm: red up, blue not");
    sc->win[0].flags &= ~(uint32_t)SC_FOCUSED;
    (void)scene_compose(sc, 0, 0, 120, 80);
    p = at(&back, 20, 40);
    check((p & 0xff) > 0x40 + 20, "a window behind, lit white whatever the warm light is");

    /* The star specular, on the window in front alone: with one white light
     * for both, the star is all that differs - an arm two from its middle,
     * at the top-left corner's 45 degrees (2 in, for a corner of 8). */
    sc->rim_front = 0xffffffffu;
    sc->win[0].plate = &plate;         /* over its corner, as a header's is */
    sc->win[0].px = 0;
    sc->win[0].py = 0;
    (void)scene_compose(sc, 0, 0, 120, 80);
    q = at(&back, 24, 22);
    sc->win[0].flags |= SC_FOCUSED;
    (void)scene_compose(sc, 0, 0, 120, 80);
    p = at(&back, 24, 22);
    check(((p >> 16) & 0xff) + ((p >> 8) & 0xff) + (p & 0xff)
          > ((q >> 16) & 0xff) + ((q >> 8) & 0xff) + (q & 0xff) + 40,
          "the window in front's corner ends in a star of light, over its plate");
    sc->win[0].flags &= ~(uint32_t)SC_FOCUSED;
    sc->win[0].plate = NULL;

    /* The split: glass whose blue is its x as well as its red, no light.
     * At the left edge red is read further in than blue, so red is the
     * higher; with no split the two are one. */
    for (y = 0; y < 80; y++) {
        for (x = 0; x < 120; x++) row_of(&frost, y)[x] = 0xff000000u | x << 16 | y << 8 | x;
    }
    sc->rim_specular = 0;
    sc->rim_split = 50;
    (void)scene_compose(sc, 0, 0, 120, 80);
    p = at(&back, 20, 40);
    check(((p >> 16) & 0xff) > (p & 0xff) + 1,
          "the split: the warm channels read further along the bend than the cool");
    sc->rim_split = 0;
    (void)scene_compose(sc, 0, 0, 120, 80);
    p = at(&back, 20, 40);
    check(((p >> 16) & 0xff) <= (p & 0xff) + 1 && (p & 0xff) <= ((p >> 16) & 0xff) + 1,
          "no split: the channels read together");

    sc->rim_bevel = 0;
    (void)scene_compose(sc, 0, 0, 120, 80);
    p = at(&back, 20, 40);
    check(!sc->lens_ok && ((p >> 16) & 0xff) <= 20 && ((p >> 16) & 0xff) + 1 >= 20,
          "without a rim, the left edge's glass unbent");

    scene_free(sc);
    free(sc);
    free(plate.pixels);
    free(back.pixels);
    free(pic.pixels);
    free(frost.pixels);
}

/*
 * **A picture from before a resize** (`SC_STALE`): a 40 by 30 picture whose
 * every pixel is its own, in a frame resized to 60 by 50. Shown at its size,
 * pixel for pixel, its last column drawn on across the new room and its
 * last row down - and without the flag, stretched, which is what the person
 * resizing saw.
 */
static void stale(void)
{
    struct surface back = make(100, 80, 0xff000000u);
    struct surface pic = make(40, 30, 0);
    struct scene *sc = malloc(sizeof(*sc));
    unsigned x, y;
    bool as_is = true;

    for (y = 0; y < 30; y++) {
        for (x = 0; x < 40; x++) {
            row_of(&pic, y)[x] = 0xff000000u | x << 16 | y << 8 | 0x5a;
        }
    }

    scene_init(sc, &back);
    sc->desktop = 0xff808080u;
    check(scene_set_windows(sc, 1) && scene_set_menus(sc, 0), "room for one window");
    sc->win[0] = (struct sc_window){ 10, 10, 60, 50, SC_STALE, &pic, 40, 30, NULL, 0, 0, 0, -1, 0, NULL, 0, 0, 0, 0, NULL, 0, 0, -1, -1, -1, -1 };
    (void)scene_compose(sc, 0, 0, 100, 80);

    for (y = 0; y < 30; y++) {
        for (x = 0; x < 40; x++) {
            as_is &= at(&back, 10 + x, 10 + y) == at(&pic, x, y);
        }
    }

    check(as_is, "a stale picture shown at its own size, pixel for pixel");
    check(at(&back, 10 + 55, 10 + 12) == at(&pic, 39, 12),
          "the new room to its right its last column drawn on");
    check(at(&back, 10 + 20, 10 + 45) == at(&pic, 20, 29),
          "the new room below its last row drawn down");
    check(at(&back, 10 + 59, 10 + 49) == at(&pic, 39, 29),
          "and the corner the last row's last pixel");
    check(at(&back, 75, 10) == 0xff808080u, "nothing drawn past the frame");

    sc->win[0].flags = 0;
    (void)scene_compose(sc, 0, 0, 100, 80);
    check(at(&back, 10 + 30, 10 + 20) != at(&pic, 30, 20),
          "without the flag it is stretched to the frame, as before");

    scene_free(sc);
    free(sc);
    free(back.pixels);
    free(pic.pixels);
}

int main(void)
{
    primitives();
    scene();
    glass();
    live();
    rim();
    beads();
    stale();

    if (failed) {
        printf("FAIL: %d of %d checks on the primitives and the scene\n", failed,
               failed + passed);
        return 1;
    }

    printf("PASS: %d checks on the primitives and the scene (fills, copies, a blend, a stretch "
           "and a triangle; two windows, a menu and a hidden one composed, a corner rounded, a "
           "drag's label, a panel by its own alpha, and the same again; a glass strip, menu and window frosted with the wallpaper's glass where they draw and not where they do not, and a glass window stretched copied; live glass - a strip and a menu over what is under them, blurred as gfx_blur blurs it, clear glass sharp, and the damage widened to hold the reach; a glass window's rim bent inward and lit toward the light - warm in front, white behind - its colours split amber from indigo, and flat without one; a picture from before a resize shown at its size, its edges drawn on)\n", passed);
    return 0;
}
