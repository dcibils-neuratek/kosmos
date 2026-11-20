/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DRIVERS_DISPLAY_AMDGPU_FW_H
#define KOSMOS_DRIVERS_DISPLAY_AMDGPU_FW_H

/*
 * **The command processor's firmware, carried in the image** (`docs/amdgpu.md`
 * V3): AMD's microcode for Tahiti and Pitcairn from linux-firmware's
 * `radeon/` - the PFP, ME and CE the CP runs, and the RLC's - put into the
 * driver's image by `.incbin` when the build names where the files are
 * (`-DAMDGPU_FW_DIR="..."`; the Makefile's, a download of its own as every
 * other is). Without it this says there is none, and the driver says the
 * firmware is not in this image and goes on without the CP.
 *
 * The files are AMD's, redistributable as linux-firmware ships them; their
 * terms go in LICENSE beside the rest.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "amdgpu_cp.h"
#include "amdgpu_decode.h"

/* The file for a chip and an engine, as bytes; false when the image has none. */
bool amdgpu_fw_image(enum amdgpu_family family, enum amdgpu_fw_kind kind,
                     const uint8_t **bytes, size_t *len);

#endif /* KOSMOS_DRIVERS_DISPLAY_AMDGPU_FW_H */
