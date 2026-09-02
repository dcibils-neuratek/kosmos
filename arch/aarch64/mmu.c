#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mmu.h"
#include "page.h"
#include "pmm.h"
#include "panic.h"
#include "hal.h"

/* Section boundaries from the linker script, all page aligned. */
extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __stack_guard[];
extern char __exception_guard[];
extern char __framebuffer_start[];

#define ENTRIES_PER_TABLE   512
#define BLOCK_2M            (2UL * 1024 * 1024)

/*
 * A 39-bit VA with a 4 KB granule splits as:
 *
 *   [38:30]  level 1 index, 1 GB per entry
 *   [29:21]  level 2 index, 2 MB per entry
 *   [20:12]  level 3 index, 4 KB per entry
 *   [11:0]   offset within the page
 */
#define L1_INDEX(va)    (((va) >> 30) & 0x1ff)
#define L2_INDEX(va)    (((va) >> 21) & 0x1ff)
#define L3_INDEX(va)    (((va) >> 12) & 0x1ff)

/*
 * The device region. Flash below 0x08000000 is deliberately left out so that
 * address 0 has no translation: a null dereference has to be a fault, not a
 * write into flash that succeeds.
 */
#define DEVICE_BASE     0x08000000UL
#define DEVICE_END      0x40000000UL

static uint64_t *kernel_l1;

static uint64_t *alloc_table(void)
{
    uint64_t *table = pmm_alloc_page();
    unsigned i;

    if (table == NULL) {
        panic("mmu: out of pages while building the page tables");
    }

    /* A page comes out of the allocator holding whatever the last owner
     * left. A stale non-zero descriptor is a mapping nobody asked for. */
    for (i = 0; i < ENTRIES_PER_TABLE; i++) {
        table[i] = 0;
    }

    return table;
}

/*
 * Replaces a 2 MB block with a table of 512 page descriptors covering the
 * same range with the same attributes. The mapping does not change; only its
 * granularity does.
 *
 * Needed because most of RAM is mapped in blocks, and a thread stack
 * allocated out of it cannot have a guard page punched into it while the
 * smallest thing describing that range is two megabytes.
 */
static uint64_t *split_block(uint64_t *entry)
{
    uint64_t *table = alloc_table();
    uint64_t base = *entry & DESC_ADDR_MASK;
    uint64_t attrs = *entry & ~(DESC_ADDR_MASK | 3);
    unsigned i;

    for (i = 0; i < ENTRIES_PER_TABLE; i++) {
        table[i] = (base + (uint64_t)i * PAGE_SIZE) | attrs | DESC_PAGE;
    }

    /*
     * The table has to be complete before the walker can be pointed at it,
     * and `dsb ishst` is what guarantees the stores have landed rather than
     * merely been ordered. Publishing a half-written table is a fault on an
     * address that was mapped a moment ago.
     */
    __asm__ volatile("dsb ishst" ::: "memory");

    *entry = (uint64_t)(uintptr_t)table | DESC_TABLE;

    /* The old block descriptor may be cached in the TLB covering all 2 MB. */
    __asm__ volatile("dsb ishst" ::: "memory");
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("dsb nsh" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    return table;
}

/* The next level down, created on demand, splitting a block if one is in the
 * way. */
static uint64_t *descend(uint64_t *table, unsigned index)
{
    uint64_t *next;

    if ((table[index] & DESC_VALID) == 0) {
        next = alloc_table();
        table[index] = (uint64_t)(uintptr_t)next | DESC_TABLE;
        return next;
    }

    if ((table[index] & 3) != DESC_TABLE) {
        return split_block(&table[index]);
    }

    return (uint64_t *)(uintptr_t)(table[index] & DESC_ADDR_MASK);
}

static void map_pages(uint64_t *root, uintptr_t va, uintptr_t pa, size_t count,
                      uint64_t attrs)
{
    size_t i;

    for (i = 0; i < count; i++) {
        uint64_t *l2 = descend(root, (unsigned)L1_INDEX(va));
        uint64_t *l3 = descend(l2, (unsigned)L2_INDEX(va));

        l3[L3_INDEX(va)] = (pa & DESC_ADDR_MASK) | attrs | DESC_PAGE;

        va += PAGE_SIZE;
        pa += PAGE_SIZE;
    }
}

static void map_blocks_2m(uint64_t *root, uintptr_t va, uintptr_t pa,
                          size_t count, uint64_t attrs)
{
    size_t i;

    for (i = 0; i < count; i++) {
        uint64_t *l2 = descend(root, (unsigned)L1_INDEX(va));

        l2[L2_INDEX(va)] = (pa & DESC_ADDR_MASK) | attrs | DESC_BLOCK;

        va += BLOCK_2M;
        pa += BLOCK_2M;
    }
}

static uint64_t *page_entry(uint64_t *root, uintptr_t va)
{
    uint64_t *l2;
    uint64_t *l3;

    if ((root[L1_INDEX(va)] & 3) != DESC_TABLE) {
        return NULL;
    }
    l2 = (uint64_t *)(uintptr_t)(root[L1_INDEX(va)] & DESC_ADDR_MASK);

    if ((l2[L2_INDEX(va)] & 3) != DESC_TABLE) {
        return NULL;    /* a 2 MB block, or nothing at all */
    }
    l3 = (uint64_t *)(uintptr_t)(l2[L2_INDEX(va)] & DESC_ADDR_MASK);

    return &l3[L3_INDEX(va)];
}

uint64_t *mmu_page_entry(uintptr_t va)
{
    return page_entry(kernel_l1, va);
}

/* Invalidates one page's translation everywhere it might be cached. */
static void invalidate(uintptr_t va)
{
    /*
     * `tlbi vaae1is` takes bits 55:12 of the address, which is why it is
     * shifted. The `is` suffix broadcasts to the inner shareable domain,
     * which costs nothing today with one core and is what SMP at M6 needs.
     */
    __asm__ volatile("dsb ishst" ::: "memory");
    __asm__ volatile("tlbi vaae1is, %0" : : "r"(va >> 12) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

static void unmap_page(uint64_t *root, uintptr_t va)
{
    /* descend rather than page_entry, because this is the caller that has to
     * work on an address currently covered by a block: that is exactly what
     * a guard page inside a freshly allocated stack is. */
    uint64_t *l2 = descend(root, (unsigned)L1_INDEX(va));
    uint64_t *l3 = descend(l2, (unsigned)L2_INDEX(va));

    l3[L3_INDEX(va)] = DESC_INVALID;
    invalidate(va);
}

void mmu_unmap_page(uintptr_t va)
{
    unmap_page(kernel_l1, va);
}


static void enable(void)
{
    uint64_t mair;
    uint64_t tcr;
    uint64_t parange;
    uint64_t sctlr;

    /*
     * MAIR_EL1 holds eight attribute bytes; a descriptor names one by index.
     *
     *   index 0 = 0x00: Device-nGnRnE. Non-gathering, non-reordering, no
     *                   early write acknowledgement. What MMIO needs.
     *   index 1 = 0xff: Normal memory, inner and outer write-back,
     *                   read-allocate and write-allocate.
     */
    mair = (0x00UL << (8 * MAIR_IDX_DEVICE))
         | (0xffUL << (8 * MAIR_IDX_NORMAL));

    /* The physical address size the CPU actually implements. Claiming more
     * than it has is a configuration fault at the first walk. */
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(parange));
    parange &= 0xf;

    tcr = (25UL << 0)       /* T0SZ: 64 - 39, a 39-bit VA for TTBR0        */
        | (1UL  << 8)       /* IRGN0: walks are inner write-back           */
        | (1UL  << 10)      /* ORGN0: walks are outer write-back           */
        | (3UL  << 12)      /* SH0: inner shareable                        */
        | (0UL  << 14)      /* TG0: 4 KB granule                           */
        | (25UL << 16)      /* T1SZ, unused but must be a legal value      */
        | (1UL  << 23)      /* EPD1: no walks through TTBR1 at all         */
        /*
         * TG1 does not use the same encoding as TG0. 4 KB is 0b00 for TTBR0
         * and 0b10 for TTBR1. Getting this wrong is a translation fault on
         * an address range that is supposed to be disabled entirely.
         */
        | (2UL  << 30)      /* TG1: 4 KB granule                           */
        | (parange << 32);  /* IPS: as much physical address as we have    */

    __asm__ volatile(
        "msr mair_el1, %0\n"
        "msr tcr_el1,  %1\n"
        "msr ttbr0_el1, %2\n"
        : : "r"(mair), "r"(tcr), "r"((uint64_t)(uintptr_t)kernel_l1)
        : "memory");

    /*
     * The tables were written as ordinary stores with translation off. The
     * table walker is a separate observer, so those stores have to have
     * completed before it can be allowed to read them. `dsb ishst` waits for
     * completion, which is what is needed here rather than mere ordering.
     */
    __asm__ volatile("dsb ishst" ::: "memory");

    /* Whatever is in the TLB predates every mapping we just built. */
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("dsb nsh" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0)     /* M: translation on                           */
          |  (1UL << 2)     /* C: the data cache, useless until M is set   */
          |  (1UL << 12);   /* I: the instruction cache                    */

    /*
     * The instruction after this one is fetched through the new translation
     * regime, so the write has to be complete before it is fetched. That is
     * exactly what isb is for. This code is identity mapped and executable,
     * which is why the fetch still lands on the instruction it should.
     */
    __asm__ volatile(
        "msr sctlr_el1, %0\n"
        "isb\n"
        : : "r"(sctlr) : "memory");
}

void mmu_init(void)
{
    uintptr_t fine_end;
    struct memrange ram;
    uintptr_t text_start   = (uintptr_t)__text_start;
    uintptr_t rodata_start = (uintptr_t)__rodata_start;
    uint64_t *guard;

    hal_ram_range(&ram);

    kernel_l1 = alloc_table();

    /* Devices, as 2 MB blocks. Nothing here is ever executed and nothing
     * here is cached. */
    map_blocks_2m(kernel_l1, DEVICE_BASE, DEVICE_BASE,
                  (DEVICE_END - DEVICE_BASE) / BLOCK_2M, MAP_DEVICE);

    /*
     * The bottom of RAM holds the kernel image, the page allocator's bitmap
     * and both stacks, and is mapped a page at a time: per section
     * permissions and an unmapped guard page both need granularity finer
     * than a block.
     *
     * **How far up is decided by the image, not by a constant.** It was the
     * first 2 MB, which was true until the image grew past it - and what
     * happens then is that the stack guards land in a 2 MB block, `page_entry`
     * cannot find a page descriptor for them, and the machine panics during
     * `mmu_init` with a message about a stack guard. The cause is somewhere
     * else entirely: something was added to the image.
     *
     * `__framebuffer_start` is the end of everything that needs fine
     * mapping - the framebuffer above it is three megabytes that only ever
     * wants to be writable - so the fine region runs to the 2 MB boundary
     * at or above it, and grows by itself the next time the image does.
     */
    fine_end = ((uintptr_t)__framebuffer_start + BLOCK_2M - 1)
               & ~(uintptr_t)(BLOCK_2M - 1);

    map_pages(kernel_l1, ram.base, ram.base,
              (fine_end - ram.base) / PAGE_SIZE, MAP_RW);

    /* Everything above it is anonymous memory and gets blocks, which is a
     * couple of hundred descriptors instead of 130,000 and a couple of
     * hundred TLB entries instead of the same. */
    map_blocks_2m(kernel_l1, fine_end, fine_end,
                  (ram.base + ram.size - fine_end) / BLOCK_2M, MAP_RW);

    /*
     * Now narrow the two regions that should not be writable. Done as a
     * second pass over an already complete map, so there is one place that
     * decides what is mapped and a separate one that decides what may be
     * done with it.
     */
    map_pages(kernel_l1, text_start, text_start,
              ((uintptr_t)__text_end - text_start) / PAGE_SIZE, MAP_TEXT);
    map_pages(kernel_l1, rodata_start, rodata_start,
              ((uintptr_t)__rodata_end - rodata_start) / PAGE_SIZE, MAP_RO);

    /* And punch out both stack guards. One below the kernel's stack, one
     * below the exception stack: a handler that overflows has to fault
     * rather than walk into whatever is underneath it. */
    guard = page_entry(kernel_l1, (uintptr_t)__stack_guard);
    if (guard == NULL) {
        panic("mmu: the stack guard is not page mapped");
    }
    *guard = DESC_INVALID;

    guard = page_entry(kernel_l1, (uintptr_t)__exception_guard);
    if (guard == NULL) {
        panic("mmu: the exception stack guard is not page mapped");
    }
    *guard = DESC_INVALID;

    enable();
}

bool mmu_is_enabled(void)
{
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    return (sctlr & 1) != 0;
}

/* ------------------------------------------------------------------ */
/* Address spaces                                                      */
/* ------------------------------------------------------------------ */

/*
 * How many address spaces there can be, which is how many processes there
 * can be - each gets exactly one.
 *
 * **This has to be at least PROCESS_MAX, and it is not checked here.** It
 * cannot be: `arch/` is "which CPU are you" and must not include a kernel
 * header, so this file cannot see that constant. The two are tied together
 * by a test instead - `as: PROCESS_MAX spaces can exist` creates that many
 * and fails if any is refused.
 *
 * That test exists because of what happened without it. This was 16 while
 * PROCESS_MAX went to 32, and the effect was a spawn that failed at eleven
 * processes with "could not spawn" while every pool the system could report
 * showed plenty free: 16 of 32 processes, 17 of 48 threads, 469 MB. A limit
 * nothing counts is a limit nobody can find.
 */
#define ADDRSPACE_MAX   32

struct addrspace {
    uint64_t *root;
    bool      in_use;
};

/* A fixed pool, like everything else in the kernel. Running out is a NULL
 * from as_create rather than a table that grows. */
static struct addrspace spaces[ADDRSPACE_MAX];

/* How many are in use, and how many there are. Reported through SYS_SYSINFO
 * for one reason: see the comment on ADDRSPACE_MAX. */
unsigned as_count(void)
{
    unsigned i, n = 0;

    for (i = 0; i < ADDRSPACE_MAX; i++) {
        if (spaces[i].in_use) {
            n++;
        }
    }

    return n;
}

unsigned as_total(void)
{
    return ADDRSPACE_MAX;
}

struct addrspace *as_create(void)
{
    unsigned i;

    for (i = 0; i < ADDRSPACE_MAX; i++) {
        if (spaces[i].in_use) {
            continue;
        }

        spaces[i].root = pmm_alloc_page();
        if (spaces[i].root == NULL) {
            return NULL;
        }

        /*
         * A copy of the kernel's top level, so the kernel is mapped here
         * too. Without it the instruction after as_switch would have no
         * translation, and the fault handler would have none either.
         *
         * The copy shares every table below it. That is what confines user
         * mappings to slots the kernel does not use: writing into one it
         * does would edit the kernel's own map, in every space at once.
         */
        for (unsigned e = 0; e < ENTRIES_PER_TABLE; e++) {
            spaces[i].root[e] = kernel_l1[e];
        }

        spaces[i].in_use = true;
        return &spaces[i];
    }

    return NULL;
}

/*
 * The maximum number of pages any user range can hold. Nothing legitimate
 * comes near it; it exists so that the multiply below cannot be made to
 * wrap.
 */
#define USER_PAGES_MAX  ((USER_VA_END - USER_VA_BASE) / PAGE_SIZE)

static bool user_range(uintptr_t va, size_t pages)
{
    /*
     * The count before it is multiplied, and this is not belt and braces.
     *
     * `pages * PAGE_SIZE` is unsigned arithmetic on a number the caller
     * chose. 2^52 + 1 pages multiplies to 4096, so an enormous request
     * wraps into a legal-looking one-page one and passes every bound below
     * it - and the caller's loop then runs `pages` times, not `end - va`
     * times, walking out of the user window and off the end of the address
     * space.
     *
     * `end > va` catches a wrap to exactly zero and nothing else, which is
     * why it looked sufficient and was not.
     */
    if (pages > USER_PAGES_MAX) {
        return false;
    }

    uintptr_t end = va + pages * PAGE_SIZE;

    return va >= USER_VA_BASE && end > va && end <= USER_VA_END;
}

int as_map(struct addrspace *as, uintptr_t va, uintptr_t pa, size_t pages,
           uint64_t attrs)
{
    size_t i;

    if ((va & PAGE_MASK) != 0 || (pa & PAGE_MASK) != 0) {
        return AS_ERR_ALIGN;
    }

    if (!user_range(va, pages)) {
        /*
         * Refused rather than allowed to work. An address below
         * USER_VA_BASE resolves through a table this space shares with the
         * kernel, so the mapping would appear in every space and in the
         * kernel's own, which is the opposite of what an address space is
         * for. It would also look like it worked.
         */
        return AS_ERR_RANGE;
    }

    for (i = 0; i < pages; i++) {
        uint64_t *l2 = descend(as->root, (unsigned)L1_INDEX(va));
        uint64_t *l3 = descend(l2, (unsigned)L2_INDEX(va));

        l3[L3_INDEX(va)] = (pa & DESC_ADDR_MASK) | attrs | DESC_PAGE;
        invalidate(va);

        va += PAGE_SIZE;
        pa += PAGE_SIZE;
    }

    return AS_OK;
}

int as_unmap(struct addrspace *as, uintptr_t va, size_t pages)
{
    size_t i;

    if ((va & PAGE_MASK) != 0) {
        return AS_ERR_ALIGN;
    }

    if (!user_range(va, pages)) {
        return AS_ERR_RANGE;
    }

    for (i = 0; i < pages; i++) {
        unmap_page(as->root, va);
        va += PAGE_SIZE;
    }

    return AS_OK;
}

uint64_t *as_page_entry(struct addrspace *as, uintptr_t va)
{
    return page_entry(as->root, va);
}

void as_switch(struct addrspace *as)
{
    uint64_t root = (uint64_t)(uintptr_t)((as != NULL) ? as->root : kernel_l1);

    __asm__ volatile("msr ttbr0_el1, %0" : : "r"(root) : "memory");
    __asm__ volatile("isb" ::: "memory");

    /*
     * The whole TLB, because there are no ASIDs yet: every entry in it
     * belongs to the space being left. Tagging spaces with an ASID is what
     * makes a switch cheap, and it is worth doing when there are processes
     * switching often, which is M4.
     */
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("dsb nsh" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

void as_destroy(struct addrspace *as)
{
    unsigned l1i;

    if (as == NULL || !as->in_use) {
        return;
    }

    /*
     * Only the slots this space owns. Everything below USER_VA_BASE is the
     * kernel's, borrowed rather than copied, and freeing it would hand the
     * kernel's own page tables back to the allocator.
     */
    for (l1i = (unsigned)L1_INDEX(USER_VA_BASE); l1i < ENTRIES_PER_TABLE; l1i++) {
        uint64_t *l2;
        unsigned l2i;

        if ((as->root[l1i] & 3) != DESC_TABLE) {
            continue;
        }

        l2 = (uint64_t *)(uintptr_t)(as->root[l1i] & DESC_ADDR_MASK);

        for (l2i = 0; l2i < ENTRIES_PER_TABLE; l2i++) {
            if ((l2[l2i] & 3) == DESC_TABLE) {
                pmm_free_page((void *)(uintptr_t)(l2[l2i] & DESC_ADDR_MASK));
            }
        }

        pmm_free_page(l2);
    }

    pmm_free_page(as->root);
    as->root = NULL;
    as->in_use = false;
}
