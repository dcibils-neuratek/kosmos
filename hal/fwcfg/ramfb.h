/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/* QEMU's ramfb, under its own name. A board decides whether to use it -
 * see `hal/pc/fb.c`, which prefers whatever the loader set up because a
 * real PC has no ramfb at all. */
#ifndef KOSMOS_HAL_FWCFG_RAMFB_H
#define KOSMOS_HAL_FWCFG_RAMFB_H

#include <stdbool.h>

struct fb;

bool ramfb_init(struct fb *out);

/* Where the size came from (`opt/kosmos/fb`, `roadmap.md` 6zt): asked for
 * and shown; the one the image was built with; or that one because what
 * was asked was no size it could show. */
enum ramfb_size { RAMFB_SIZE_BUILT, RAMFB_SIZE_ASKED, RAMFB_SIZE_REFUSED };

enum ramfb_size ramfb_size_from(void);

#endif
