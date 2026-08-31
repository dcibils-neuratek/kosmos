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

/*
 * And the two a process gets.
 *
 * The AP field is what isolation actually rests on. Everything above uses
 * AP=00, meaning EL1 read/write and **no EL0 access at all**, so a process
 * cannot touch kernel memory whether or not the kernel is mapped in its
 * address space. That is why the kernel living in TTBR0 alongside processes
 * is a layout decision rather than a security one.
 *
 * UXN is set on user data and PXN on user code, and the second matters more
 * than it looks: without it the kernel could be tricked into executing bytes
 * a process wrote. A page is executable by exactly one exception level.
 */
#define ATTR_AP_RW_EL0  (1UL << 6)      /* EL1 rw, EL0 rw */
#define ATTR_AP_RO_EL0  (3UL << 6)      /* EL1 ro, EL0 ro */

#define MAP_USER_RW (ATTR_IDX(MAIR_IDX_NORMAL) | ATTR_AF | ATTR_SH_INNER | \
                     ATTR_AP_RW_EL0 | ATTR_PXN | ATTR_UXN)

#define MAP_USER_RX (ATTR_IDX(MAIR_IDX_NORMAL) | ATTR_AF | ATTR_SH_INNER | \
                     ATTR_AP_RO_EL0 | ATTR_PXN)   /* UXN clear: EL0 executes */

/* Builds the identity map and turns translation on. Needs pmm_init first,
 * because the tables come out of the page allocator. */
void mmu_init(void);

/* Whether SCTLR_EL1.M is set. */
bool mmu_is_enabled(void);

/* Removes the mapping for one page, splitting a 2 MB block first if the
 * address is inside one. Used for guard pages under freshly allocated
 * stacks, which land in the part of RAM mapped as blocks. */
void mmu_unmap_page(uintptr_t va);

/* The level 3 descriptor for a virtual address, or NULL when the address is
 * not described by a page-granular mapping. For tests and inspection. */
uint64_t *mmu_page_entry(uintptr_t va);

/*
 * Address spaces.
 *
 * A space is a page table root and the mappings hanging off it. Creating
 * one, adding pages to it, and switching to it is what a process will be
 * built out of at M4; until there is something to run at EL0 it is a
 * mechanism with tests and no users, which is the honest state to leave it
 * in rather than pretending otherwise.
 *
 * **Every space contains the kernel.** There is no TTBR1 split yet: the
 * kernel is identity mapped through TTBR0 like everything else, so a space
 * that did not contain it would fault on the instruction after the switch.
 * A new space therefore starts as a copy of the kernel's top level and adds
 * to it.
 *
 * That copy shares the levels below it, which is why user mappings are
 * confined to their own region: writing into a range the kernel already
 * describes would edit the kernel's map through the shared table, in every
 * space at once. The split is enforced rather than documented.
 *
 * At M4 this is replaced by the real arrangement, where the kernel lives in
 * TTBR1 at the top of the address space and TTBR0 belongs entirely to the
 * process. Then a space contains no kernel at all, which is the point.
 */

/*
 * Where a space may map things: level 1 slot 2 upwards. The kernel occupies
 * slots 0 and 1, being devices below 1 GB and RAM from 1 to 2 GB.
 */
/*
 * Address spaces in use, and how many there can be. Exposed because a pool
 * that nothing counts is a limit nobody can find: this one was the real
 * ceiling on processes for a while and no report mentioned it.
 */
unsigned as_count(void);
unsigned as_total(void);

#define USER_VA_BASE    0x80000000UL
#define USER_VA_END     (512UL * 1024 * 1024 * 1024)    /* a 39-bit VA */

struct addrspace;

/* A new space containing the kernel and nothing else. NULL when the pool is
 * full or there are no pages for the table. */
struct addrspace *as_create(void);

/* Frees the space and every table it allocated. Not the kernel's, which it
 * only borrowed. Switching to a destroyed space is not detected, so do not. */
void as_destroy(struct addrspace *as);

/* Maps `pages` pages. Fails on a virtual address outside the user region,
 * on a misaligned address, or when there are no pages for the tables. */
int as_map(struct addrspace *as, uintptr_t va, uintptr_t pa, size_t pages,
           uint64_t attrs);

/* Removes a mapping. The pages themselves are not freed: the space did not
 * allocate them and does not know who did. */
int as_unmap(struct addrspace *as, uintptr_t va, size_t pages);

/* Makes this space the one TTBR0 describes. Passing NULL restores the
 * kernel's. */
void as_switch(struct addrspace *as);

/* The level 3 descriptor for an address in this space, or NULL. For tests
 * and inspection. */
uint64_t *as_page_entry(struct addrspace *as, uintptr_t va);

#define AS_OK           0
#define AS_ERR_RANGE   (-1)     /* outside the user region */
#define AS_ERR_ALIGN   (-2)     /* not page aligned */
#define AS_ERR_NOMEM   (-3)

#endif /* ARCH_AARCH64_MMU_H */
