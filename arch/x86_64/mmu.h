/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef ARCH_X86_64_MMU_H
#define ARCH_X86_64_MMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Paging for x86-64, and the counterpart of `arch/aarch64/mmu.h`.
 *
 * Four levels rather than three, and that is the one structural difference
 * the rest of this file keeps having to answer for. AArch64 was told to use
 * a 39-bit virtual address, which drops a level; x86-64 in long mode has no
 * such switch - PAE paging is four levels and that is the only paging long
 * mode has.
 *
 *   PML4 [47:39]   512 GB per entry
 *   PDPT [38:30]     1 GB per entry
 *   PD   [29:21]     2 MB per entry
 *   PT   [20:12]     4 KB per entry
 *
 * The bottom three are the ARM side's level 1, 2 and 3 exactly, which is
 * why the code below reads so nearly the same. Everything this system maps
 * lives under 512 GB, so **PML4 has one live entry** and the extra level is
 * a page and an indirection rather than a different design.
 *
 * The map is an identity map, for the reason the ARM side gives: a printed
 * address is the address.
 *
 * What it deliberately does NOT map:
 *
 *   - Page 0, so a null dereference is a page fault naming address 0.
 *   - The stack guard page, so an overflow is a fault rather than a quiet
 *     write into .bss.
 */

/* Entry bits. Intel SDM volume 3, table 4-19 and the ones around it. */
#define PTE_P           (1UL << 0)      /* present */
#define PTE_RW          (1UL << 1)      /* writable */
#define PTE_US          (1UL << 2)      /* reachable from ring 3 */
#define PTE_PWT         (1UL << 3)      /* write-through */
#define PTE_PCD         (1UL << 4)      /* cache disabled */
#define PTE_A           (1UL << 5)      /* accessed */
#define PTE_D           (1UL << 6)      /* dirty */
#define PTE_PS          (1UL << 7)      /* a large page, at PD and PDPT */
#define PTE_G           (1UL << 8)      /* global: survives a CR3 reload */
#define PTE_NX          (1UL << 63)     /* no execute; needs EFER.NXE */

/*
 * The output address, bits 51:12.
 *
 * Wider than AArch64's 47:12 because x86-64 defines 52 physical address
 * bits where ARMv8 defines 48. Nothing here has that much memory; the mask
 * is the architecture's, not the machine's.
 */
#define PTE_ADDR_MASK   0x000ffffffffff000UL

/*
 * **Permissions accumulate down the walk, and this is the difference from
 * AArch64 that matters most.**
 *
 * On ARM a table descriptor's attributes are optional *restrictions* which
 * default to permissive, so only the leaf normally says anything. On x86 a
 * page is writable only if PTE_RW is set at every one of the four levels,
 * and reachable from ring 3 only if PTE_US is set at every one. An
 * intermediate entry that forgot US makes every user page beneath it
 * invisible, and the fault says "not present" while the page table plainly
 * contains the page.
 *
 * So the rule this file follows: **intermediate entries are permissive and
 * leaves carry the policy.** A PML4, PDPT or PD entry that points at a
 * table is always P|RW|US, and what a page may be used for is decided once,
 * in the entry that describes the page. That is what Linux does and for the
 * same reason: any other arrangement means a permission is stored in four
 * places and can disagree with itself.
 */
#define TABLE_ATTRS     (PTE_P | PTE_RW | PTE_US)

/*
 * The four combinations the kernel uses.
 *
 * Caching is where the ARM comparison stops being useful. MAIR_EL1 holds
 * eight named memory types and a descriptor picks one by index; x86 has two
 * bits per entry that select one of four PAT slots, and the defaults out of
 * reset make PCD|PWT mean uncached. Device memory takes both, which is
 * Device-nGnRnE's nearest equivalent and is what it is used for.
 */
#define MAP_DEVICE  (PTE_P | PTE_RW | PTE_PCD | PTE_PWT | PTE_NX)
#define MAP_RW      (PTE_P | PTE_RW | PTE_NX)
#define MAP_RO      (PTE_P | PTE_NX)
#define MAP_TEXT    (PTE_P)                     /* read-only and executable */

/*
 * And the two a process gets.
 *
 * PTE_US is what isolation rests on: every mapping above has it clear, so
 * ring 3 cannot touch kernel memory whether or not the kernel is mapped in
 * the same address space. The kernel living in every space is a layout
 * decision rather than a security one - the same sentence the ARM header
 * writes about AP=00.
 *
 * **One difference is a real weakening and is not papered over here.**
 * AArch64 has two execute-never bits, PXN and UXN, so a page can be
 * executable at EL0 and not at EL1. x86-64 has one NX bit for both, so
 * MAP_USER_RX is executable by the kernel too. The architecture's answer is
 * CR4.SMEP, which faults when ring 0 executes a user page, and `mmu.c`
 * turns it on where the processor has it - so the guarantee is the same one
 * and it is bought with a control register instead of a descriptor bit.
 *
 * SMAP, its counterpart for *reads*, is deliberately not enabled: the
 * kernel reads user memory on every IPC, and doing that under SMAP means
 * bracketing each access with STAC and CLAC. That is a real piece of work
 * rather than a bit to set, and it belongs with the syscall path.
 */
#define MAP_USER_RW (PTE_P | PTE_RW | PTE_US | PTE_NX)
#define MAP_USER_RX (PTE_P | PTE_US)

/* Builds the identity map and loads it. Needs pmm_init first, because the
 * tables come out of the page allocator. */
void mmu_init(void);

/* Whether paging is on. */
bool mmu_is_enabled(void);

/* Removes the mapping for one page, splitting a 2 MB page first if the
 * address is inside one. Used for guard pages under freshly allocated
 * stacks, which land in the part of RAM mapped as large pages. */
void mmu_unmap_page(uintptr_t va);

/* The PT entry for a virtual address, or NULL when the address is not
 * described by a page-granular mapping. For tests and inspection. */
uint64_t *mmu_page_entry(uintptr_t va);

/*
 * Address spaces.
 *
 * **Every space contains the kernel**, exactly as on ARM and for the same
 * reason: there is no split like TTBR1 yet, so a space without the kernel
 * in it would fault on the instruction after the switch, and the handler
 * would have no translation either.
 *
 * A new space is therefore a copy of the level that holds the user slots,
 * sharing every table below it - which is what confines user mappings to
 * their own region, since writing into a range the kernel already describes
 * would edit the kernel's map in every space at once.
 *
 * On ARM that copied level is the L1 root and a space costs one page. Here
 * it is the PDPT, because that is the level whose slots the user region
 * divides - so a space costs two: its own PML4, whose single live entry
 * points at its own PDPT.
 */
unsigned as_count(void);
unsigned as_total(void);

#define USER_VA_BASE    0x80000000UL
#define USER_VA_END     (512UL * 1024 * 1024 * 1024)    /* one PML4 slot */

struct addrspace;

/* A new space containing the kernel and nothing else. NULL when the pool is
 * full or there are no pages for the tables. */
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

/* Makes this space the one CR3 describes. Passing NULL restores the
 * kernel's. */
void as_switch(struct addrspace *as);

/* The PT entry for an address in this space, or NULL. For tests and
 * inspection. */
uint64_t *as_page_entry(struct addrspace *as, uintptr_t va);

#define AS_OK           0
#define AS_ERR_RANGE   (-1)     /* outside the user region */
#define AS_ERR_ALIGN   (-2)     /* not page aligned */
#define AS_ERR_NOMEM   (-3)

#endif /* ARCH_X86_64_MMU_H */
