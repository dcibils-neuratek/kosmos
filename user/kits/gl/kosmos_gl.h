/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_GL_H
#define KOSMOS_GL_H

/*
 * The GL Kit's door for C (`docs/windowkit.md`, the IDE's examples, 7
 * October): a TinyGL program in a window of its own.
 *
 *   #include <GL/gl.h>
 *   #include "ui.h"                 TinyGL's examples' five functions
 *   #include "kosmos_gl.h"
 *
 *   const char *why = kosmos_gl_run("Gears", 400, 300,
 *                                   init, draw, idle, reshape, key);
 *
 * Opens the window (the Window Kit), makes a TinyGL context its size, calls
 * `init` and `reshape`, then every frame `idle` and `draw` and copies the
 * picture into the window - until it is closed, by its close box or by
 * Escape. Arrows reach `key` as `ui.h`'s KEY_UP and the rest; a resize
 * makes a context the new size and calls `reshape`. Answers NULL, or why the
 * window would not open.
 *
 * So a program written to TinyGL's `ui.h` - its own demos, unchanged - runs
 * here with one call, and nothing of the window is its business.
 */

#include <GL/gl.h>

const char *kosmos_gl_run(const char *title, int width, int height,
                          void (*init)(void), void (*draw)(void),
                          void (*idle)(void), void (*reshape)(int, int),
                          GLenum (*key)(int));

#endif /* KOSMOS_GL_H */
