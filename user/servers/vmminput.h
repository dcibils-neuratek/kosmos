/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef USER_SERVERS_VMMINPUT_H
#define USER_SERVERS_VMMINPUT_H

/*
 * **A sandbox's keyboard and pointer** (`docs/linux.md` L3): two
 * virtio-input devices on virtio-mmio (VIRTIO 1.2 §5.8) - a keyboard and an
 * absolute tablet - fed from the ring in its screen region's first page
 * (`vmmscreen.h`), which its window writes. There with a screen, and only
 * then: the window that shows the picture is what has the keys.
 */

#include <stdbool.h>
#include <stdint.h>

/* The devices, reading the ring in the screen region's first page. */
bool vmminput_start(void *screen_page);

/* Events the window wrote, into buffers the guest has given: as many as
 * there are buffers for, in order. Called whenever the monitor looks. */
void vmminput_pump(void);

bool vmminput_on(void);
bool vmminput_mmio(unsigned i, uint64_t off, unsigned size, bool write, uint64_t *value);
bool vmminput_raised(unsigned i);

#endif /* USER_SERVERS_VMMINPUT_H */
