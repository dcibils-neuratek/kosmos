#ifndef ARCH_AARCH64_MMU_H
#define ARCH_AARCH64_MMU_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Stage 1 translation for EL1.
 *
 * 4 KB granule, 39-bit virtual addresses, TTBR0 only. That gives three
 * levels instead of four: level 1 entries cover 1 GB, level 2 entries 2 MB,
 * level 3 entries 4 KB. 512 GB of address space is far more than anything
 * here will use, and dropping a level removes a table walk from every miss.
 *
 * The map is an identity map. Virtual equals physical everywhere, because
 * until there are user processes at M4 there is nothing to gain from them
 * differing, and an identity map means a printed address is the address.
 *
 * What it deliberately does NOT map:
 *
 *   - The first 128 MB, so a null dereference is a translation fault naming
 *     address 0 rather than a quiet write into flash.
 *   - The stack guard page, so an overflow is an abort rather than silent
 *     corruption of .bss.
 */

/* Descriptor types. Bits [1:0]. */
#define DESC_INVALID    0UL
#define DESC_BLOCK      1UL     /* levels 1 and 2: a 1 GB or 2 MB block */
#define DESC_TABLE      3UL     /* levels 0 to 2: points at the next level */
#define DESC_PAGE       3UL     /* level 3: a 4 KB page */
#define DESC_VALID      1UL

/* The output address sits in bits [47:12] for every descriptor kind. */
#define DESC_ADDR_MASK  0x0000fffffffff000UL

/* Lower attributes, bits [11:2]. */
#define ATTR_IDX(n)     ((uint64_t)(n) << 2)    /* which MAIR_EL1 entry */
#define ATTR_AP_RW_EL1  (0UL << 6)              /* EL1 read/write, EL0 none */
#define ATTR_AP_RO_EL1  (2UL << 6)              /* EL1 read-only, EL0 none */
#define ATTR_SH_INNER   (3UL << 8)              /* inner shareable */
/*
 * The access flag. Hardware does not set it for you unless the CPU
 * implements hardware AF management and it is turned on, so a descriptor
 * without it faults on first touch with an "access flag fault". It is the
 * single most common reason a first page table produces a fault that makes
 * no sense.
 */
#define ATTR_AF         (1UL << 10)

/* Upper attributes. */
#define ATTR_PXN        (1UL << 53)     /* never executable at EL1 */
#define ATTR_UXN        (1UL << 54)     /* never executable at EL0 */

/* MAIR_EL1 indices, defined by mmu.c when it programs the register. */
#define MAIR_IDX_DEVICE 0               /* Device-nGnRnE */
#define MAIR_IDX_NORMAL 1               /* Normal, write-back, read/write allocate */

/* The three combinations the kernel actually uses. */
#define MAP_DEVICE  (ATTR_IDX(MAIR_IDX_DEVICE) | ATTR_AF | ATTR_AP_RW_EL1 | \
                     ATTR_PXN | ATTR_UXN)
#define MAP_RW      (ATTR_IDX(MAIR_IDX_NORMAL) | ATTR_AF | ATTR_SH_INNER | \
                     ATTR_AP_RW_EL1 | ATTR_PXN | ATTR_UXN)
#define MAP_RO      (ATTR_IDX(MAIR_IDX_NORMAL) | ATTR_AF | ATTR_SH_INNER | \
                     ATTR_AP_RO_EL1 | ATTR_PXN | ATTR_UXN)
#define MAP_TEXT    (ATTR_IDX(MAIR_IDX_NORMAL) | ATTR_AF | ATTR_SH_INNER | \
                     ATTR_AP_RO_EL1 | ATTR_UXN)   /* PXN clear: executable */

/* Builds the identity map and turns translation on. Needs pmm_init first,
 * because the tables come out of the page allocator. */
void mmu_init(void);

/* Whether SCTLR_EL1.M is set. */
bool mmu_is_enabled(void);

/* The level 3 descriptor for a virtual address, or NULL when the address is
 * not described by a page-granular mapping. For tests and inspection. */
uint64_t *mmu_page_entry(uintptr_t va);

#endif /* ARCH_AARCH64_MMU_H */
