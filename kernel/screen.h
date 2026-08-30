#ifndef KERNEL_SCREEN_H
#define KERNEL_SCREEN_H

#include <stdbool.h>

#include "hal.h"

/*
 * What display the board found at boot, remembered.
 *
 * The kernel does not draw and has no opinion about pixels. This exists for
 * one reason: a process that is granted the screen needs those pages mapped
 * into its address space, and the granting happens in `process.c` long after
 * `kmain` asked the board what it had.
 *
 * `hal_fb_init` cannot simply be called again to find out. It clears the
 * framebuffer, which is right once at boot and wrong every time after -
 * calling it to read the geometry would wipe whatever was on screen.
 *
 * Exactly one process may hold the screen at a time, for the same reason
 * exactly one holds the console: a device with two owners is a device with
 * no owner. Today `init` hands it to the shell; at the app server it hands
 * it there instead, and nothing else about this changes.
 */

/* Called once by kmain, with what the board reported. */
void screen_init(const struct fb *fb);

/* The geometry, or false when the machine booted without a display. */
bool screen_get(struct fb *out);

#endif /* KERNEL_SCREEN_H */
