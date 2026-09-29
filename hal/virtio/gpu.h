/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * virtio-gpu, its 2D half (`roadmap.md` 4h a). A board decides whether to
 * use it, as it decides about ramfb.
 */
#ifndef KOSMOS_HAL_VIRTIO_GPU_H
#define KOSMOS_HAL_VIRTIO_GPU_H

#include <stdbool.h>

struct fb;

/* The screen as a resource scanned out of guest pages; false when the
 * machine has no virtio-gpu, or it would not take one. */
bool virtio_gpu_init(struct fb *out);

/* Whether `virtio_gpu_init` said yes: the board's flush goes here then. */
bool virtio_gpu_present(void);

/* What was drawn in that rectangle, to the device and onto the screen. */
void virtio_gpu_flush(unsigned x, unsigned y, unsigned w, unsigned h);

#endif
