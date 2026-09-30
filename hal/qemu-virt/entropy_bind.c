/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Where this board's randomness comes from, and there is only one answer:
 * virtio-rng, when QEMU was given one. A Cortex-A72 is ARMv8.0 and has no
 * RNDR, so there is no instruction to ask first. The same shape as
 * `blk_bind.c`, for the reason it gives: the choice between sources belongs
 * to the board, and here there is nothing to choose between.
 */

#include <stdbool.h>
#include <stddef.h>

#include "hal.h"
#include "rng.h"

bool hal_entropy_init(void)
{
    return virtio_rng_init();
}

size_t hal_entropy(void *buf, size_t bytes)
{
    return virtio_rng_read(buf, bytes);
}

const char *hal_entropy_describe(void)
{
    return virtio_rng_present() ? "virtio-rng"
                                : "no virtio-rng device, and no RNDR on this processor";
}
