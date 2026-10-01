/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * virtio-gpu, its 2D half (`roadmap.md` 4h a). A board decides whether to
 * use it, as it decides about ramfb.
 */
#ifndef KOSMOS_HAL_VIRTIO_GPU_H
#define KOSMOS_HAL_VIRTIO_GPU_H

#include <stdbool.h>
#include <stdint.h>

struct fb;

/* The screen as a resource scanned out of guest pages; false when the
 * machine has no virtio-gpu, or it would not take one. */
bool virtio_gpu_init(struct fb *out);

/* Whether `virtio_gpu_init` said yes: the board's flush goes here then. */
bool virtio_gpu_present(void);

/* What was drawn in that rectangle, to the device and onto the screen. */
void virtio_gpu_flush(unsigned x, unsigned y, unsigned w, unsigned h);

/* The pointer drawn by the device (`roadmap.md` 4h b): a 64 by 64 picture
 * of 0xAARRGGBB words and its hot spot, then positions. */
bool virtio_gpu_cursor_set(const uint32_t *argb, unsigned hot_x, unsigned hot_y,
                           unsigned x, unsigned y);
void virtio_gpu_cursor_move(unsigned x, unsigned y);
void virtio_gpu_cursor_hide(void);

#endif
