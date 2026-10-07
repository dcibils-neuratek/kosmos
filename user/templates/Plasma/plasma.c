/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * plasma.c - a C app with a window of its own, through the Window Kit
 * (`kosmos_window.h`): an animated plasma, drawn into the window's pixels
 * every frame.
 *
 *   Space      pause and go on
 *   a click    the next palette
 *   Escape     close, as the close box does
 *
 * The window can be resized; the next frame is drawn at the new size.
 */

#include <math.h>
#include <stdio.h>

#include "kosmos_kit.h"
#include "kosmos_window.h"

static uint8_t wave[256];               /* a sine, 0..255 around */
static uint32_t palette[256];

static void make_palette(int which)
{
    for (int i = 0; i < 256; i++) {
        double a = i * 2 * 3.14159265358979 / 256;
        int r = (int)(128 + 127 * sin(a + which));
        int g = (int)(128 + 127 * sin(a + which * 2 + 2.1));
        int b = (int)(128 + 127 * sin(a + which * 3 + 4.2));

        palette[i] = 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
    }
}

static void draw(struct kw_surface s, unsigned t)
{
    for (unsigned y = 0; y < s.height; y++) {
        uint32_t *row = (uint32_t *)((char *)s.pixels + (size_t)y * s.pitch);
        unsigned wy = wave[(y * 3 + t * 2) & 255];

        for (unsigned x = 0; x < s.width; x++) {
            unsigned v = wave[(x * 2 + t) & 255] + wy
                       + wave[(x + y + t * 3) & 255]
                       + wave[((x * x + y * y) / 512 + t) & 255];

            row[x] = palette[(v / 4) & 255];
        }
    }
}

/* main(): the window until it is closed; then how many frames, in words. */
static int l_main(lua_State *L)
{
    struct kw_window *w = kw_open("Plasma", 480, 300, KW_RESIZABLE | KW_CENTRE);
    struct kw_event e;
    unsigned t = 0, frames = 0;
    int paused = 0, which = 0;
    char said[96];

    if (w == NULL) {
        lua_pushfstring(L, "plasma: no window: %s", kw_why());
        return 1;
    }

    for (int i = 0; i < 256; i++) {
        wave[i] = (uint8_t)(128 + 127 * sin(i * 2 * 3.14159265358979 / 256));
    }

    make_palette(which);

    for (;;) {
        struct kw_surface s = kw_surface(w);

        if (s.pixels != NULL) {
            draw(s, t);
            kw_commit(w, 0, 0, s.width, s.height);
            frames++;
        }

        if (!paused) {
            t++;
        }

        while (kw_poll(w, &e, paused ? 100 : 16)) {
            if (e.type == KW_CLOSE || (e.type == KW_KEY && e.key == 27)) {
                kw_close(w);
                snprintf(said, sizeof said, "plasma: %u frames", frames);
                lua_pushstring(L, said);
                return 1;
            }

            if (e.type == KW_KEY && e.key == ' ') {
                paused = !paused;
            }

            if (e.type == KW_POINTER && e.action == KW_PRESS) {
                make_palette(++which);
            }
        }
    }
}

KOSMOS_KIT(plasma)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_main);
    lua_setfield(L, -2, "main");
}
