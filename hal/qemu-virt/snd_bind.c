/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Where this board's sound comes from: virtio, and there is no second
 * answer.
 *
 * A pass-through, and - exactly as with `input_bind.c` - the indirection
 * earns its place on the other board rather than on this one. A PC has an
 * Intel HDA controller soldered to it and a virtio device only when QEMU
 * was told to add one, so the PC has to choose; `virt` has one sound device
 * in the world and this file says so in eight lines.
 *
 * `hal/pc/snd_bind.c` is the same file with a real decision in it.
 */

#include <stdbool.h>

#include "hal.h"
#include "snd.h"

bool     hal_snd_init(void)    { return virtio_snd_init(); }
bool     hal_snd_present(void) { return virtio_snd_present(); }
unsigned hal_snd_queued(void)  { return virtio_snd_queued(); }

bool hal_snd_write(const void *pcm, unsigned bytes)
{
    return virtio_snd_write(pcm, bytes);
}

bool     hal_snd_wants(void)   { return virtio_snd_wants(); }
unsigned hal_snd_wakes(void)   { return virtio_snd_wakes(); }
unsigned hal_snd_dry(void)     { return virtio_snd_dry(); }
unsigned hal_snd_floor(void)   { return virtio_snd_floor(); }

void snd_interrupt(unsigned line) { virtio_snd_interrupt(line); }

const char *hal_snd_describe(void)
{
    return "virtio-sound";
}
