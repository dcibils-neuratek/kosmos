/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Intel High Definition Audio: the sound controller a PC actually has.
 *
 * **This is the first driver in the tree written for hardware rather than
 * for QEMU.** Everything else on this board either exists on both - the
 * i8042, the PIC, the PIT - or is QEMU's own and has no counterpart on a
 * laptop, which is what `ramfb` is. HDA is neither: it is a real
 * specification that Intel has shipped in every chipset since 2004, and
 * QEMU emulates it well enough to develop against. `docs/thinkpad.md` says
 * why that combination is worth more than either half.
 *
 * Under its own names for `hal/virtio/snd.h`'s reason: a board binds the
 * HAL, and this board has two possible sources of sound.
 */
#ifndef KOSMOS_HAL_PC_HDA_H
#define KOSMOS_HAL_PC_HDA_H

#include <stdbool.h>

bool     hda_init(void);
bool     hda_present(void);
bool     hda_write(const void *pcm, unsigned bytes);
unsigned hda_queued(void);

bool     hda_wants(void);
unsigned hda_wakes(void);

unsigned hda_dry(void);
unsigned hda_floor(void);

void     hda_interrupt(unsigned line);

/*
 * What the controller and its codec turned out to be, for the boot log.
 *
 * The same argument `hal_fb_describe` makes: on a machine with no serial
 * port, the difference between "no HDA controller" and "a controller with
 * no codec on it" and "a codec with no output pin" is three different
 * faults with three different fixes, and they all present as silence.
 */
const char *hda_describe(void);

#endif
