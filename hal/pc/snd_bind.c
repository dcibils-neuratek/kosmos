/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Where this board's sound comes from: the HDA controller first, then
 * virtio.
 *
 * **The same shape as `fb.c` and for the same reason.** A ThinkPad has an
 * Intel HDA controller on its PCI bus and no virtio anything; a machine
 * booted under QEMU has whichever of the two it was told to add, and
 * usually both are available. Asking for the real one first costs nothing
 * when it is not there - `pci_find_class` walks the bus and answers no -
 * and is the difference between sound on hardware and silence.
 *
 * Neither driver knows the other exists, which is the whole point of the
 * HAL names belonging to a board rather than to a driver.
 */

#include <stdbool.h>

#include "hal.h"
#include "hda.h"
#include "snd.h"

static bool on_hda;

bool hal_snd_init(void)
{
    if (hda_init()) {
        on_hda = true;
        return true;
    }

    return virtio_snd_init();
}

bool hal_snd_present(void)
{
    return on_hda ? hda_present() : virtio_snd_present();
}

unsigned hal_snd_queued(void)
{
    return on_hda ? hda_queued() : virtio_snd_queued();
}

bool hal_snd_write(const void *pcm, unsigned bytes)
{
    return on_hda ? hda_write(pcm, bytes) : virtio_snd_write(pcm, bytes);
}

bool     hal_snd_wants(void) { return on_hda ? hda_wants() : virtio_snd_wants(); }
unsigned hal_snd_wakes(void) { return on_hda ? hda_wakes() : virtio_snd_wakes(); }
unsigned hal_snd_dry(void)   { return on_hda ? hda_dry()   : virtio_snd_dry(); }
unsigned hal_snd_floor(void) { return on_hda ? hda_floor() : virtio_snd_floor(); }

/*
 * Offered to both, for the reason `input_bind.c` gives: a PCI line is
 * shared and the number alone does not say who raised one. Only one of them
 * was initialised, and the other returns immediately because it has no
 * device - so this is a branch that could be written and is not worth it.
 */
void snd_interrupt(unsigned line)
{
    hda_interrupt(line);
    virtio_snd_interrupt(line);
}

/*
 * And which of them it was.
 *
 * When virtio answered, that is the whole story. When it did not, the
 * interesting sentence is the HDA driver's - it is the one that knows
 * whether there was no controller, no codec on it, or no way out of the
 * codec - so a machine with no sound at all reports the real reason rather
 * than the last one tried.
 */
const char *hal_snd_describe(void)
{
    if (on_hda) {
        return hda_describe();
    }

    return virtio_snd_present() ? "virtio-sound" : hda_describe();
}
