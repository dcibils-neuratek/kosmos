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

static uint64_t *l1_table;

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

static void map_pages(uintptr_t va, uintptr_t pa, size_t count, uint64_t attrs)
{
    size_t i;

    for (i = 0; i < count; i++) {
        uint64_t *l2 = descend(l1_table, (unsigned)L1_INDEX(va));
        uint64_t *l3 = descend(l2, (unsigned)L2_INDEX(va));

        l3[L3_INDEX(va)] = (pa & DESC_ADDR_MASK) | attrs | DESC_PAGE;

        va += PAGE_SIZE;
        pa += PAGE_SIZE;
    }
}

static void map_blocks_2m(uintptr_t va, uintptr_t pa, size_t count,
                          uint64_t attrs)
{
    size_t i;

    for (i = 0; i < count; i++) {
        uint64_t *l2 = descend(l1_table, (unsigned)L1_INDEX(va));

        l2[L2_INDEX(va)] = (pa & DESC_ADDR_MASK) | attrs | DESC_BLOCK;

        va += BLOCK_2M;
        pa += BLOCK_2M;
    }
}

uint64_t *mmu_page_entry(uintptr_t va)
{
    uint64_t *l2;
    uint64_t *l3;

    if ((l1_table[L1_INDEX(va)] & 3) != DESC_TABLE) {
        return NULL;
    }
    l2 = (uint64_t *)(uintptr_t)(l1_table[L1_INDEX(va)] & DESC_ADDR_MASK);

    if ((l2[L2_INDEX(va)] & 3) != DESC_TABLE) {
        return NULL;    /* a 2 MB block, or nothing at all */
    }
    l3 = (uint64_t *)(uintptr_t)(l2[L2_INDEX(va)] & DESC_ADDR_MASK);

    return &l3[L3_INDEX(va)];
}

void mmu_unmap_page(uintptr_t va)
{
    /* descend rather than mmu_page_entry, because this is the caller that
     * has to work on an address currently covered by a block: that is
     * exactly what a guard page inside a freshly allocated stack is. */
    uint64_t *l2 = descend(l1_table, (unsigned)L1_INDEX(va));
    uint64_t *l3 = descend(l2, (unsigned)L2_INDEX(va));

    l3[L3_INDEX(va)] = DESC_INVALID;

    /*
     * Invalidating by address rather than the whole TLB. `tlbi vaae1is`
     * takes bits 55:12 of the address, which is why it is shifted. The `is`
     * suffix broadcasts to the inner shareable domain, which costs nothing
     * today with one core and is what SMP at M6 needs.
     */
    __asm__ volatile("dsb ishst" ::: "memory");
    __asm__ volatile("tlbi vaae1is, %0" : : "r"(va >> 12) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
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
        : : "r"(mair), "r"(tcr), "r"((uint64_t)(uintptr_t)l1_table)
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
    struct memrange ram;
    uintptr_t text_start   = (uintptr_t)__text_start;
    uintptr_t rodata_start = (uintptr_t)__rodata_start;
    uint64_t *guard;

    hal_ram_range(&ram);

    l1_table = alloc_table();

    /* Devices, as 2 MB blocks. Nothing here is ever executed and nothing
     * here is cached. */
    map_blocks_2m(DEVICE_BASE, DEVICE_BASE,
                  (DEVICE_END - DEVICE_BASE) / BLOCK_2M, MAP_DEVICE);

    /*
     * The first 2 MB of RAM holds the kernel image, the page allocator's
     * bitmap and the boot stack, so it is mapped a page at a time: per
     * section permissions and an unmapped guard page both need granularity
     * finer than a block.
     */
    map_pages(ram.base, ram.base, BLOCK_2M / PAGE_SIZE, MAP_RW);

    /* Everything above it is anonymous memory and gets blocks, which is 255
     * descriptors instead of 130,000 and 255 TLB entries instead of the
     * same. */
    map_blocks_2m(ram.base + BLOCK_2M, ram.base + BLOCK_2M,
                  (ram.size - BLOCK_2M) / BLOCK_2M, MAP_RW);

    /*
     * Now narrow the two regions that should not be writable. Done as a
     * second pass over an already complete map, so there is one place that
     * decides what is mapped and a separate one that decides what may be
     * done with it.
     */
    map_pages(text_start, text_start,
              ((uintptr_t)__text_end - text_start) / PAGE_SIZE, MAP_TEXT);
    map_pages(rodata_start, rodata_start,
              ((uintptr_t)__rodata_end - rodata_start) / PAGE_SIZE, MAP_RO);

    /* And punch out both stack guards. One below the kernel's stack, one
     * below the exception stack: a handler that overflows has to fault
     * rather than walk into whatever is underneath it. */
    guard = mmu_page_entry((uintptr_t)__stack_guard);
    if (guard == NULL) {
        panic("mmu: the stack guard is not page mapped");
    }
    *guard = DESC_INVALID;

    guard = mmu_page_entry((uintptr_t)__exception_guard);
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
