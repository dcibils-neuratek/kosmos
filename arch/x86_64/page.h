/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef ARCH_X86_64_PAGE_H
#define ARCH_X86_64_PAGE_H

/*
 * The translation granule.
 *
 * x86-64 has one and it is 4 KB. There are large pages - 2 MB and 1 GB, and
 * the boot path uses 2 MB ones to map the first gigabyte before there is an
 * allocator - but those are a property of an *entry*, not a choice of
 * granule the way AArch64's 16 KB and 64 KB are. The bottom level is 4 KB
 * on every x86-64 that exists.
 *
 * Which is why this file is three lines shorter than its AArch64 twin and
 * says the same thing: the numbers happen to agree, and they agree for
 * different reasons.
 */

#define PAGE_SHIFT  12
#define PAGE_SIZE   (1UL << PAGE_SHIFT)
#define PAGE_MASK   (PAGE_SIZE - 1)

#define PAGE_ALIGN_DOWN(x)  ((x) & ~PAGE_MASK)
#define PAGE_ALIGN_UP(x)    PAGE_ALIGN_DOWN((x) + PAGE_MASK)

#endif /* ARCH_X86_64_PAGE_H */
