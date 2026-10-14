/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef USER_SERVERS_VMMGPU_H
#define USER_SERVERS_VMMGPU_H

/*
 * **A sandbox's screen** (`docs/linux.md` L2): virtio-gpu, 2D only, on
 * virtio-mmio (VIRTIO 1.2 §5.7) - one display, the size the screen region
 * says, each rectangle the guest flushes laid into that region as Kosmos's
 * own pixels (`vmmscreen.h`) for a window to draw. No 3D: Chromium draws in
 * software, which is the first version's bargain.
 */

#include <stdbool.h>
#include <stdint.h>

/* The screen region `screen_cap`; false, and no display offered, when it is
 * not one (`VMM_SCREEN_MAGIC`, a width a multiple of 16, room for it). */
bool vmmgpu_start(long screen_cap);

bool vmmgpu_on(void);

/* The screen region's first page, where its input ring is (`vmminput.h`). */
void *vmmgpu_page(void);
bool vmmgpu_mmio(uint64_t off, unsigned size, bool write, uint64_t *value);
bool vmmgpu_raised(void);

#endif /* USER_SERVERS_VMMGPU_H */
