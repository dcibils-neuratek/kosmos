/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef HAL_VIRTIO_RNG_H
#define HAL_VIRTIO_RNG_H

#include <stdbool.h>
#include <stddef.h>

/*
 * virtio-rng: randomness from the host, under QEMU on both boards
 * (`hal_entropy`, the boards' `entropy_bind.c`).
 */
bool   virtio_rng_init(void);
bool   virtio_rng_present(void);
size_t virtio_rng_read(void *buf, size_t bytes);

#endif /* HAL_VIRTIO_RNG_H */
