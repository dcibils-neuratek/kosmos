/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef USER_INCLUDE_VMMPROTO_H
#define USER_INCLUDE_VMMPROTO_H

/*
 * **What a sandbox's monitor is handed** - `docs/virtualization.md` V3.
 *
 * `vmm` is started with three capabilities: the console its guest speaks
 * on, a region that is the guest's RAM with its kernel (and initramfs)
 * already in it, and a page holding one of these. Whoever starts it - the
 * `sandbox` program, through `fs.vm_start` - reads the files into the RAM
 * region itself, so a kernel's bytes go from the disk server into a region
 * and are never a Lua string.
 *
 * Offsets are into the RAM region, which the guest sees at `VMM_RAM_BASE`.
 */

#include <stdint.h>

#define VMM_SETUP_MAGIC     0x4b766d6du     /* "Kvmm" */
#define VMM_SETUP_VERSION   1u
#define VMM_CMDLINE_MAX     512u

/* Where things are in every guest's physical address space - QEMU `virt`'s
 * map, which Linux has no opinion about and a reader of a boot log does. */
#define VMM_RAM_BASE        0x40000000ul
#define VMM_GICD_BASE       0x08000000ul
#define VMM_GICR_BASE       0x080a0000ul
#define VMM_UART_BASE       0x09000000ul

struct vmm_setup {
    uint32_t magic;
    uint32_t version;
    uint64_t ram_bytes;                 /* the region's size, its pages' */
    uint64_t kernel_offset;             /* an arm64 Image, 2 MB aligned */
    uint64_t kernel_bytes;
    uint64_t initrd_offset;             /* 0 bytes: none */
    uint64_t initrd_bytes;
    char     cmdline[VMM_CMDLINE_MAX];
};

_Static_assert(sizeof(struct vmm_setup) == 560, "six words, a magic, a version and a line");

#endif /* USER_INCLUDE_VMMPROTO_H */
