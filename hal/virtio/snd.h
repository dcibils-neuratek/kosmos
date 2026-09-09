/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The virtio sound driver, under its own names.
 *
 * **A board binds these to the HAL, rather than the driver being the HAL.**
 * The reason is `hal/virtio/input.h`'s reason with a different device: the
 * PC has an Intel HDA controller now, and a machine that takes its sound
 * from one driver on hardware and another under emulation cannot have two
 * files defining the same eight symbols.
 *
 * `hal/pc/snd_bind.c` is that machine, and `hal/qemu-virt/snd_bind.c` is
 * the board where the answer is still "virtio, for everything".
 */
#ifndef KOSMOS_HAL_VIRTIO_SND_H
#define KOSMOS_HAL_VIRTIO_SND_H

#include <stdbool.h>

bool     virtio_snd_init(void);
bool     virtio_snd_present(void);
bool     virtio_snd_write(const void *pcm, unsigned bytes);
unsigned virtio_snd_queued(void);

bool     virtio_snd_wants(void);
unsigned virtio_snd_wakes(void);

unsigned virtio_snd_dry(void);
unsigned virtio_snd_floor(void);

void     virtio_snd_interrupt(unsigned line);

#endif
