#ifndef ARCH_AARCH64_PAGE_H
#define ARCH_AARCH64_PAGE_H

/*
 * The translation granule.
 *
 * AArch64 offers 4 KB, 16 KB and 64 KB. Kosmos uses 4 KB: it is what every
 * piece of reference material assumes, it is what the Pi firmware hands over,
 * and a larger granule only starts paying off with address spaces far bigger
 * than anything here will have.
 *
 * It lives in arch/ because it is a property of the CPU's page tables, not a
 * choice the kernel is free to make.
 */

#define PAGE_SHIFT  12
#define PAGE_SIZE   (1UL << PAGE_SHIFT)
#define PAGE_MASK   (PAGE_SIZE - 1)

#define PAGE_ALIGN_DOWN(x)  ((x) & ~PAGE_MASK)
#define PAGE_ALIGN_UP(x)    PAGE_ALIGN_DOWN((x) + PAGE_MASK)

#endif /* ARCH_AARCH64_PAGE_H */
