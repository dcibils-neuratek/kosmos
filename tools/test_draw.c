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

    sc->win[0] = (struct sc_window){ 10, 10, 100, 60, SC_ROUNDED, &w1, 100, 60, NULL, 0, 0, 0, -1, -1, -1 };
    sc->win[1] = (struct sc_window){ 60, 40, 100, 60, SC_ROUNDED, &w2, 100, 60, NULL, 0, 0, 0, -1, -1, -1 };
    sc->win[2] = (struct sc_window){ 150, 5, 40, 40, SC_HIDDEN, &w1, 100, 60, NULL, 0, 0, 0, -1, -1, -1 };
    sc->menu[0] = (struct sc_menu){ 140, 80, 40, 30, &menu, -1 };

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
    sc->win[0] = (struct sc_window){ 10, 10, 60, 50, SC_STALE, &pic, 40, 30, NULL, 0, 0, 0, -1, -1, -1 };
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
    stale();

    if (failed) {
        printf("FAIL: %d of %d checks on the primitives and the scene\n", failed,
               failed + passed);
        return 1;
    }

    printf("PASS: %d checks on the primitives and the scene (fills, copies, a blend, a stretch "
           "and a triangle; two windows, a menu and a hidden one composed, a corner rounded, a "
           "drag's label, a panel by its own alpha, and the same again; a picture from before a resize shown at its size, its edges drawn on)\n", passed);
    return 0;
}
