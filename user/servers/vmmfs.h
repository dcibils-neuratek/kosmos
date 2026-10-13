/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef USER_SERVERS_VMMFS_H
#define USER_SERVERS_VMMFS_H

/*
 * A sandbox's folders, each a virtio-fs device on virtio-mmio
 * (`vmmfs.c`, `docs/sandboxes.md` S3). `vmm.c` adds them, says where the
 * guest's RAM is, sends each one's register accesses here, and asks which
 * want their interrupt.
 */

#include <stdbool.h>
#include <stdint.h>

#define VMMFS_MOST      4u

bool vmmfs_add(unsigned i, const char *tag, const char *root, bool read_only);
void vmmfs_start(long home_cap);
bool vmmfs_mmio(unsigned i, uint64_t off, unsigned size, bool write, uint64_t *value);
bool vmmfs_raised(unsigned i);

#endif /* USER_SERVERS_VMMFS_H */
