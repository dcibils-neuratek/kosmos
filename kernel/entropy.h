/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KERNEL_ENTROPY_H
#define KERNEL_ENTROPY_H

#include <stdbool.h>
#include <stddef.h>

/* The hardware's randomness, held to a health test (`entropy.c`). */
bool   entropy_init(void);
size_t entropy_read(void *buf, size_t bytes);

/* The most one syscall hands out. */
#define ENTROPY_MAX 256u

#endif /* KERNEL_ENTROPY_H */
