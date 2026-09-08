/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Lite XL surface shim, checked on this machine rather than the target.
 *
 * `tools/test_kfs.lua` and `tools/test_wav.lua` are here for the same
 * reason and it is worth restating: a thing that depends on nothing but C
 * should be tested without booting a machine, because a test that costs
 * thirty seconds and an emulator is a test somebody runs less often.
 *
 * `user/lib/litexl_sdl.c` needs `stdlib.h` and `string.h` and nothing else -
 * no syscalls, no Kosmos headers, no framebuffer - so it compiles with the
 * host compiler exactly as it does with the cross one. That property is
 * worth keeping: it is what makes this file possible, and losing it would
 * be a real cost rather than a detail.
 *
 * **Compiling is not working.** `make litexl` says the port's C builds;
 * this says the part of it Kosmos wrote is correct. They are different
 * claims and the first was starting to be read as the second.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <stdlib.h>

#include "../user/lib/litexl/SDL.h"
#include "../runtime/upstream/lite-xl/src/renderer.h"

static int checks;
static int failures;

static void ok(bool cond, const char *what)
{
    checks++;

    if (!cond) {
        failures++;
        printf("not ok %d - %s\n", checks, what);
    }
}

/* The layout a Kosmos surface has: 0xAARRGGBB. */
#define RM 0x00FF0000u
#define GM 0x0000FF00u
#define BM 0x000000FFu
#define AM 0xFF000000u

static uint32_t at(SDL_Surface *s, int x, int y)
{
    return *(uint32_t *)((uint8_t *)s->pixels + (size_t)y * (size_t)s->pitch
                         + (size_t)x * 4u);
}

int main(void)
{
    /*------------------------------------------------ formats and colours */
    {
        SDL_Surface *s = SDL_CreateRGBSurface(0, 8, 4, 32, RM, GM, BM, AM);
        uint8_t r, g, b, a;

        ok(s != NULL, "a 32-bit surface is made");
        ok(s->w == 8 && s->h == 4, "it is the size that was asked for");
        ok(s->pitch == 32, "pitch is width times four when nobody said otherwise");
        ok(s->format->BytesPerPixel == 4, "four bytes a pixel");

        /* The shifts are derived from the masks, which is the part worth
         * checking: `renderer.c` takes pixels apart with them by hand. */
        ok(s->format->Rshift == 16 && s->format->Gshift == 8
           && s->format->Bshift == 0 && s->format->Ashift == 24,
           "the shifts follow the masks");

        ok(SDL_MapRGB(s->format, 0x12, 0x34, 0x56) == 0xFF123456u,
           "MapRGB is opaque, as SDL's is");
        ok(SDL_MapRGBA(s->format, 0x12, 0x34, 0x56, 0x78) == 0x78123456u,
           "MapRGBA keeps the alpha it was given");

        SDL_GetRGBA(0x78123456u, s->format, &r, &g, &b, &a);
        ok(r == 0x12 && g == 0x34 && b == 0x56 && a == 0x78,
           "GetRGBA is MapRGBA backwards");

        SDL_FreeSurface(s);
    }

    /* A depth that is not 32 is refused rather than approximated. */
    ok(SDL_CreateRGBSurface(0, 8, 8, 16, RM, GM, BM, AM) == NULL,
       "a 16-bit surface is refused");
    ok(SDL_CreateRGBSurface(0, 0, 8, 32, RM, GM, BM, AM) == NULL,
       "a surface with no width is refused");

    /*----------------------------------------------------- rectangles */
    {
        SDL_Rect a = { 0, 0, 10, 10 };
        SDL_Rect b = { 5, 5, 10, 10 };
        SDL_Rect c = { 40, 40, 2, 2 };
        SDL_Rect out;

        ok(SDL_IntersectRect(&a, &b, &out), "overlapping rectangles meet");
        ok(out.x == 5 && out.y == 5 && out.w == 5 && out.h == 5,
           "and the overlap is where it should be");
        ok(!SDL_IntersectRect(&a, &c, &out), "distant rectangles do not");
    }

    /*----------------------------------------------------------- filling */
    {
        SDL_Surface *s = SDL_CreateRGBSurface(0, 8, 8, 32, RM, GM, BM, AM);
        SDL_Rect     r = { 2, 2, 3, 3 };
        SDL_Rect     clip = { 0, 0, 4, 4 };

        SDL_FillRect(s, &r, 0xFF112233u);
        ok(at(s, 2, 2) == 0xFF112233u && at(s, 4, 4) == 0xFF112233u,
           "a fill covers its rectangle");
        ok(at(s, 1, 2) == 0u && at(s, 5, 4) == 0u,
           "and not a pixel outside it");

        /* The clip rectangle is what `renwin_set_clip_rect` sets, and the
         * whole reason the editor can draw a line without disturbing the
         * rest of the window. */
        SDL_SetClipRect(s, &clip);
        SDL_FillRect(s, &r, 0xFF445566u);
        ok(at(s, 3, 3) == 0xFF445566u, "a fill inside the clip lands");
        ok(at(s, 4, 4) == 0xFF112233u, "and stops at the clip's edge");

        SDL_SetClipRect(s, NULL);
        ok(s->clip_rect.w == 8 && s->clip_rect.h == 8,
           "NULL puts the clip back to the whole surface");

        SDL_FillRect(s, NULL, 0xFF778899u);
        ok(at(s, 0, 0) == 0xFF778899u && at(s, 7, 7) == 0xFF778899u,
           "a NULL rectangle fills all of it");

        SDL_FreeSurface(s);
    }

    /*------------------------------------------------------- the one blit */
    {
        /*
         * `ren_draw_rect`'s shape exactly: a one-pixel surface stretched
         * over a rectangle. It is how Lite XL fills, and the only caller
         * `SDL_BlitScaled` has.
         */
        SDL_Surface *dst = SDL_CreateRGBSurface(0, 8, 8, 32, RM, GM, BM, AM);
        SDL_Surface *one = SDL_CreateRGBSurface(0, 1, 1, 32, RM, GM, BM, AM);
        SDL_Rect     to  = { 1, 1, 4, 4 };

        SDL_FillRect(dst, NULL, 0xFF000000u);

        *(uint32_t *)one->pixels = 0xFF00FF00u;      /* opaque green */
        SDL_BlitScaled(one, NULL, dst, &to);

        ok(at(dst, 1, 1) == 0xFF00FF00u && at(dst, 4, 4) == 0xFF00FF00u,
           "an opaque stretch covers its rectangle");
        ok(at(dst, 0, 0) == 0xFF000000u && at(dst, 5, 5) == 0xFF000000u,
           "and nothing outside it");

        /*
         * Half alpha over black is half the colour. If the alpha were
         * ignored - which is the easy mistake - this would read 0x00FF00
         * and every translucent overlay in the editor would be opaque.
         */
        SDL_FillRect(dst, NULL, 0xFF000000u);
        *(uint32_t *)one->pixels = 0x80FFFFFFu;
        SDL_BlitScaled(one, NULL, dst, &to);

        {
            uint32_t p = at(dst, 2, 2);
            unsigned r = (p >> 16) & 0xFFu;

            ok(r >= 0x7Eu && r <= 0x81u, "half alpha blends about halfway");
            ok((p & AM) == AM, "and the result stays opaque");
        }

        /* Fully transparent leaves what was there. */
        SDL_FillRect(dst, NULL, 0xFF010203u);
        *(uint32_t *)one->pixels = 0x00FFFFFFu;
        SDL_BlitScaled(one, NULL, dst, &to);
        ok(at(dst, 2, 2) == 0xFF010203u, "zero alpha changes nothing");

        SDL_FreeSurface(one);
        SDL_FreeSurface(dst);
    }

    /*------------------------------------------------- the window's pixels */
    {
        /*
         * The arrangement that makes this a `direct` window: the surface is
         * a *view* onto memory the Lua side owns, so what the editor draws
         * is already in the window's buffer and no copy happens anywhere.
         */
        static uint32_t owned[16 * 4];
        SDL_Surface    *s;
        SDL_Rect        got[8];
        SDL_Rect        r1 = { 1, 2, 3, 4 };
        bool            whole;
        int             n, i;

        memset(owned, 0, sizeof owned);
        litexl_window_attach(owned, 16, 4, 16 * 4);

        s = SDL_GetWindowSurface(litexl_window());
        ok(s != NULL, "a window has a surface once one is attached");
        ok(s->pixels == owned, "and it is the caller's memory, not a copy");
        ok(s->w == 16 && s->h == 4, "at the size it was given");

        SDL_FillRect(s, NULL, 0xFFABCDEFu);
        ok(owned[0] == 0xFFABCDEFu,
           "so drawing lands in the window's own buffer");

        /* Attaching says everything changed, because nothing is drawn yet. */
        n = litexl_damage_take(got, 8, &whole);
        ok(whole, "a freshly attached window is damaged in full");

        SDL_UpdateWindowSurfaceRects(litexl_window(), &r1, 1);
        n = litexl_damage_take(got, 8, &whole);
        ok(n == 1 && !whole, "one rectangle comes back as one rectangle");
        ok(got[0].x == 1 && got[0].y == 2 && got[0].w == 3 && got[0].h == 4,
           "unchanged");

        ok(litexl_damage_take(got, 8, &whole) == 0,
           "and taking it empties the list");

        /*
         * Past the bound the answer is "all of it". A compositor handed two
         * hundred rectangles is slower than one handed the window, so the
         * overflow is a decision rather than a failure.
         */
        for (i = 0; i < 200; i++) {
            SDL_UpdateWindowSurfaceRects(litexl_window(), &r1, 1);
        }

        litexl_damage_take(got, 8, &whole);
        ok(whole, "more damage than fits says the whole window");

        SDL_DestroyWindow(litexl_window());
        ok(SDL_GetWindowSurface(litexl_window()) == NULL,
           "and destroying it takes the surface away");

        /* The foreign pixels survive, which is what `owns_pixels` is for. */
        ok(owned[0] == 0xFFABCDEFu, "without freeing memory it did not own");
    }

    /*----------------------------------------------------- the renderer */
    {
        /*
         * **A real font, rasterised, and pixels checked.** Everything above
         * tests the surface; this tests that text arrives on it, which is
         * the whole of step four and the part that `make litexl` cannot
         * see - it reports that the port *compiles*, and compiling has
         * never been the same claim as working.
         *
         * The font is read with `fopen` because this runs on the build
         * machine. On the target there is no such call and the bytes come
         * from the Lua side through `litexl_font_provide`, which is the
         * function being exercised either way.
         */
        static const char *const path =
            "assets/fonts/IBMPlexSans-Regular.ttf";
        FILE          *fp = fopen(path, "rb");
        unsigned char *bytes;
        long           len;
        RenFont       *font;
        RenFont       *group[FONT_FALLBACK_MAX];
        SDL_Surface   *s;
        static uint32_t canvas[64 * 32];
        int             i;

        ok(fp != NULL, "the test font is where it is expected");

        if (fp != NULL) {
            fseek(fp, 0, SEEK_END);
            len = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            bytes = malloc((size_t)len);
            ok(fread(bytes, 1, (size_t)len, fp) == (size_t)len,
               "and it reads");
            fclose(fp);

            litexl_font_provide(path, bytes, (size_t)len);

            /* Lite XL asks by a path that does not exist here, so the match
             * is on the file's name. That is the case worth checking. */
            font = ren_font_load(NULL, "/lite-xl/fonts/IBMPlexSans-Regular.ttf",
                                 16.0f, FONT_ANTIALIASING_GRAYSCALE,
                                 FONT_HINTING_SLIGHT, 0);
            ok(font != NULL, "a font loads by its name, not its whole path");

            ok(ren_font_load(NULL, "NoSuchFont.ttf", 16.0f,
                             FONT_ANTIALIASING_GRAYSCALE,
                             FONT_HINTING_SLIGHT, 0) == NULL,
               "and one nobody provided does not");

            for (i = 0; i < FONT_FALLBACK_MAX; i++) { group[i] = NULL; }
            group[0] = font;

            ok(ren_font_group_get_height(group) > 8
               && ren_font_group_get_height(group) < 40,
               "the line height is plausible for 16 pixels");
            ok(ren_font_group_get_size(group) == 16.0f, "and the size is 16");

            {
                double one = ren_font_group_get_width(NULL, group, "i", 1, NULL);
                double many = ren_font_group_get_width(NULL, group,
                                                       "iiiiiiiiii", 10, NULL);

                ok(one > 0, "a glyph has a width");
                ok(many > one * 5, "and ten of them are wider than one");
            }

            /* Now draw, and look. */
            litexl_window_attach(canvas, 64, 32, 64 * 4);
            s = SDL_GetWindowSurface(litexl_window());
            ok(s != NULL, "the window has a surface to draw into");

            SDL_FillRect(s, NULL, 0xFF000000u);

            {
                RenSurface rs = { s, 1 };
                double     end;
                int        lit = 0;
                int        x, y;

                end = ren_draw_text(&rs, group, "Hi", 2, 2.0f, 2,
                                    (RenColor){ .r = 255, .g = 255, .b = 255, .a = 255 });

                ok(end > 2.0, "drawing advances the pen");

                for (y = 0; y < 32; y++) {
                    for (x = 0; x < 64; x++) {
                        if (at(s, x, y) != 0xFF000000u) { lit++; }
                    }
                }

                ok(lit > 0, "and puts ink on the surface");

                /* Clipped drawing must stay inside the clip. */
                SDL_FillRect(s, NULL, 0xFF000000u);
                {
                    SDL_Rect clip = { 0, 0, 64, 4 };
                    int      below = 0;

                    SDL_SetClipRect(s, &clip);
                    ren_draw_text(&rs, group, "Hi", 2, 2.0f, 2,
                                  (RenColor){ .r = 255, .g = 255, .b = 255, .a = 255 });
                    SDL_SetClipRect(s, NULL);

                    for (y = 4; y < 32; y++) {
                        for (x = 0; x < 64; x++) {
                            if (at(s, x, y) != 0xFF000000u) { below++; }
                        }
                    }

                    ok(below == 0, "text does not draw past its clip");
                }

                /* ren_draw_rect, both ways. */
                SDL_FillRect(s, NULL, 0xFF000000u);
                /*
                 * **Named, because `RenColor` is `{ b, g, r, a }`.**
                 *
                 * Blue first, which is not what anybody writing
                 * `{0x10, 0x20, 0x30, 255}` means - the first version of
                 * this check asked for one colour and tested for another,
                 * and the renderer was right both times. Designated
                 * initialisers cost nothing and make the trap unsteppable.
                 */
                ren_draw_rect(&rs, (RenRect){ 1, 1, 4, 4 },
                              (RenColor){ .r = 0x10, .g = 0x20, .b = 0x30,
                                          .a = 255 });
                ok(at(s, 2, 2) == 0xFF102030u, "an opaque rect is its colour");

                SDL_FillRect(s, NULL, 0xFF000000u);
                ren_draw_rect(&rs, (RenRect){ 1, 1, 4, 4 },
                              (RenColor){ .r = 255, .g = 255, .b = 255, .a = 128 });
                {
                    unsigned r = (at(s, 2, 2) >> 16) & 0xFFu;

                    ok(r >= 0x7Eu && r <= 0x81u,
                       "and a translucent one blends");
                }
            }

            ren_font_free(font);
            SDL_DestroyWindow(litexl_window());
            free(bytes);
        }
    }

    /*-------------------------------------------- the two that are algorithm */
    {
        /*
         * `system`'s other thirty functions are questions for a server and
         * cannot be checked without a machine. These two are loops over
         * bytes, and `user/lib/litexl_match.c` keeps them away from Lua so
         * that they can be checked here instead.
         */
        int score, worse;

        ok(litexl_fuzzy_match("readme.md", "rme", false, &score),
           "letters in order match, with gaps");
        ok(!litexl_fuzzy_match("readme.md", "zq", false, &score),
           "letters that are not there do not");
        ok(!litexl_fuzzy_match("abc", "abcd", false, &score),
           "and a needle longer than its haystack does not");

        /* Consecutive letters must beat the same letters scattered. */
        litexl_fuzzy_match("readme", "rea", false, &score);
        litexl_fuzzy_match("roeoaom", "rea", false, &worse);
        ok(score > worse, "a run scores better than the same letters spread");

        /* Exact case beats the wrong case, which is what makes typing
         * `Init` find `Init` before `init`. */
        litexl_fuzzy_match("Init", "In", false, &score);
        litexl_fuzzy_match("init", "In", false, &worse);
        ok(score > worse, "matching case scores higher");

        ok(litexl_path_before("a", true, "b", false),
           "a directory sorts before a file");
        ok(!litexl_path_before("b", false, "a", true),
           "and a file does not sort before a directory");
        ok(litexl_path_before("apple", false, "Banana", false),
           "names compare without regard to case");
        ok(!litexl_path_before("same", false, "same", false),
           "and equal is not before");
    }

    if (failures == 0) {
        printf("PASS: %d checks on the Lite XL shim and renderer, on this machine.\n",
               checks);
        return 0;
    }

    printf("FAIL: %d of %d checks on the Lite XL shim and renderer.\n",
           failures, checks);
    return 1;
}
