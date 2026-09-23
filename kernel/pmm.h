/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KERNEL_PMM_H
#define KERNEL_PMM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* How many usable ranges the allocator will ask the board for. A PC's map
 * has two pieces of ordinary memory, below and above the PCI hole. */
#define PMM_RANGES_MAX 8

/* Reads every usable RAM range from the HAL, spans one bitmap across them
 * with the gaps between marked taken, places that bitmap after the kernel
 * image, and marks everything the kernel already occupies as used. */
void pmm_init(void);

/* One page, or NULL when there are none left. The contents are whatever the
 * previous owner left; nothing is zeroed. */
void *pmm_alloc_page(void);

/* A run of `count` physically contiguous pages, or NULL. Needed by anything
 * that has to be one span in physical memory: Lua's heap, and from M3 the
 * per-thread kernel stacks.
 *
 * Freed one page at a time, like any other. The allocator does not remember
 * that a run was a run, because nothing needs it to and remembering would
 * mean a second data structure to keep consistent with the first. */
void *pmm_alloc_contiguous(size_t count);

/* Hands a page back. Panics on a misaligned address, an address outside RAM,
 * or a double free, because all three are programmer errors that would
 * otherwise corrupt the bitmap and surface much later. */
void pmm_free_page(void *page);

/* Pages currently available, and pages the allocator manages in total. The
 * difference between total and free at boot is the kernel image plus the
 * bitmap itself. */
size_t pmm_free_pages(void);
size_t pmm_total_pages(void);

/* The pages at the bottom that only the kernel's own allocations may reach,
 * and whether a program may be given this many without eating them. See
 * `pmm.c`: this is what replaced every per-process cap on memory. */
size_t pmm_reserve_pages(void);
bool   pmm_room_for_user(size_t pages);

/* Where the memory this allocator owns begins. On a PC that is wherever
 * the firmware left the largest usable block, not a constant. */
uintptr_t pmm_ram_base(void);

#endif /* KERNEL_PMM_H */
