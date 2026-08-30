#ifndef KERNEL_PMM_H
#define KERNEL_PMM_H

#include <stddef.h>

/*
 * The physical page allocator.
 *
 * A bitmap over usable RAM, one bit per 4 KB page. Everything the kernel
 * ever allocates physically comes from here: page tables, stacks, and from
 * M4 the per-process Lua heaps.
 *
 * A bitmap rather than a free list because it is inspectable. A corrupt free
 * list is a wild pointer chase; a corrupt bitmap is a value you can print.
 * At this scale the cost of scanning is irrelevant next to that.
 *
 * There is no allocator of arbitrary sizes here and there will not be one in
 * the kernel. Fixed-size pools and whole pages, per CLAUDE.md.
 */

/* Reads the RAM range from the HAL, places the bitmap after the kernel
 * image, and marks everything the kernel already occupies as used. */
void pmm_init(void);

/* One page, or NULL when there are none left. The contents are whatever the
 * previous owner left; nothing is zeroed. */
void *pmm_alloc_page(void);

/* Hands a page back. Panics on a misaligned address, an address outside RAM,
 * or a double free, because all three are programmer errors that would
 * otherwise corrupt the bitmap and surface much later. */
void pmm_free_page(void *page);

/* Pages currently available, and pages the allocator manages in total. The
 * difference between total and free at boot is the kernel image plus the
 * bitmap itself. */
size_t pmm_free_pages(void);
size_t pmm_total_pages(void);

#endif /* KERNEL_PMM_H */
