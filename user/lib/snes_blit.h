/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_SNES_BLIT_H
#define KOSMOS_SNES_BLIT_H

/*
 * The Super Nintendo's picture into a surface, at a whole-number scale.
 *
 * Its own file, apart from `snes_kosmos.c`, so it can be tested on the host
 * with no core and no ROM: it is the only code between the picture the core
 * draws and the window a person sees, and `--scale 2` is exactly the kind of
 * change that is right in the middle and wrong at an edge.
 *
 * Nearest neighbour, on purpose. A console's pixels are meant to be seen as
 * blocks, and anything smoother would be inventing detail the game never had.
 */

#include <stdint.h>

/* 2 fills 1024 by 960, which fits under the bar on a 1080-line screen; 3
 * would not fit on any screen this system has run on. */
#define SNES_SCALE_MAX  2u

/*
 * `src` is `sw` by `sh` pixels, `sw` of them a row, as the core writes them.
 * Each becomes a `scale` by `scale` block of `dst`, whose rows are `pitch`
 * bytes apart; nothing outside `w` by `h` is written, and every pixel written
 * is opaque, because a surface blends on its alpha and nothing guarantees the
 * core writes one.
 *
 * A scale of 0 is taken as 1. A `pitch` narrower than a row of `w` pixels is
 * refused by writing nothing: the rows would overlap.
 */
void snes_blit(uint32_t *dst, unsigned w, unsigned h, unsigned pitch,
               const uint32_t *src, unsigned sw, unsigned sh, unsigned scale);

#endif /* KOSMOS_SNES_BLIT_H */
