/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/* QEMU's ramfb, under its own name. A board decides whether to use it -
 * see `hal/pc/fb.c`, which prefers whatever the loader set up because a
 * real PC has no ramfb at all. */
#ifndef KOSMOS_HAL_FWCFG_RAMFB_H
#define KOSMOS_HAL_FWCFG_RAMFB_H

#include <stdbool.h>

struct fb;

bool ramfb_init(struct fb *out);

#endif
