/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The GL Kit's door for C (`kosmos_gl.h`): a TinyGL program, written to
 * TinyGL's examples' `ui.h`, in a window of its own - the Window Kit's
 * window and TinyGL's context, as `gl_kosmos.c` gives them to Lua.
 */

#include <stdint.h>
#include <string.h>

#include <GL/gl.h>
#include <GL/ostinygl.h>

#include "kosmos_gl.h"
#include "../window/kosmos_window.h"

/* `ui.h`'s keys, which the demos switch on. */
#define UI_KEY_UP      0xe000
#define UI_KEY_DOWN    0xe001
#define UI_KEY_LEFT    0xe002
#define UI_KEY_RIGHT   0xe003

/* Kosmos's arrows (`keys.lua`): Up -1, Down -2, Right -3, Left -4. */
static int ui_key(int k)
{
    switch (k) {
    case -1: return UI_KEY_UP;
    case -2: return UI_KEY_DOWN;
    case -3: return UI_KEY_RIGHT;
    case -4: return UI_KEY_LEFT;
    default: return k;
    }
}

/*
 * What `ui.h` promises a demo of a backend, and what Kosmos gives it.
 *
 * `swap_buffers` does nothing: TinyGL has rendered into its own buffer by
 * the time it is called, and the copy into a window's surface is made by
 * whoever runs the demo - `kosmos_gl_run` below, or the system's demos'
 * `gl.blit` - when it is ready. `ui_loop` is never called: a demo's `main`
 * is renamed away (`-Dmain=...`), but that `main` still names it.
 */
void swap_buffers(void);
void swap_buffers(void) { }

int ui_loop(int argc, char **argv, const char *name);
int ui_loop(int argc, char **argv, const char *name)
{
    (void)argc; (void)argv; (void)name;
    return 0;
}

/* TinyGL's picture into the window's surface, row by row at its pitch. */
static void copy(ostgl_context_t *ctx, int w, int h, struct kw_surface s)
{
    const uint32_t *src = (const uint32_t *)ostgl_convert_framebuffer(ctx);
    int rows = h < (int)s.height ? h : (int)s.height;
    int cols = w < (int)s.width ? w : (int)s.width;

    if (src == NULL) {
        return;
    }

    for (int y = 0; y < rows; y++) {
        memcpy((uint8_t *)s.pixels + (size_t)y * s.pitch, src + (size_t)y * w,
               (size_t)cols * sizeof(uint32_t));
    }
}

const char *kosmos_gl_run(const char *title, int width, int height,
                          void (*init)(void), void (*draw)(void),
                          void (*idle)(void), void (*reshape)(int, int),
                          GLenum (*key)(int))
{
    struct kw_window *w = kw_open(title, (unsigned)width, (unsigned)height,
                                  KW_RESIZABLE | KW_CENTRE);
    ostgl_context_t *ctx;
    struct kw_event e;

    if (w == NULL) {
        return kw_why();
    }

    ctx = ostgl_create_context(width, height, 32);

    if (ctx == NULL) {
        kw_close(w);
        return "no memory for a GL context that size";
    }

    ostgl_make_current(ctx);
    init();
    reshape(width, height);

    for (;;) {
        struct kw_surface s = kw_surface(w);

        idle();
        draw();

        if (s.pixels != NULL) {
            copy(ctx, width, height, s);
            kw_commit(w, 0, 0, s.width, s.height);
        }

        while (kw_poll(w, &e, 10)) {
            if (e.type == KW_CLOSE || (e.type == KW_KEY && e.key == 27)) {
                ostgl_delete_context(ctx);
                kw_close(w);
                return NULL;
            }

            if (e.type == KW_KEY && key != NULL) {
                key(ui_key(e.key));
            }

            if (e.type == KW_RESIZE && e.width > 0 && e.height > 0) {
                ostgl_context_t *bigger = ostgl_create_context(e.width, e.height, 32);

                if (bigger != NULL) {
                    ostgl_delete_context(ctx);
                    ctx = bigger;
                    width = e.width;
                    height = e.height;
                    ostgl_make_current(ctx);
                    reshape(width, height);
                }
            }
        }
    }
}
