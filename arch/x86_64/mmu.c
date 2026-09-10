/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mmu.h"
#include "page.h"
#include "pmm.h"
#include "spinlock.h"
#include "panic.h"
#include "hal.h"

/* Section boundaries from the linker script, all page aligned. */
extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __stack_guard[];
extern char __exception_guard[];
extern char __framebuffer_start[];
extern char __image_end[];

#define ENTRIES_PER_TABLE   512
#define BLOCK_2M            (2UL * 1024 * 1024)

/*
 * A 48-bit VA with a 4 KB granule splits as:
 *
 *   [47:39]  PML4 index, 512 GB per entry
 *   [38:30]  PDPT index,   1 GB per entry
 *   [29:21]  PD   index,   2 MB per entry
 *   [20:12]  PT   index,   4 KB per entry
 *   [11:0]   offset within the page
 */
#define PML4_INDEX(va)  (((va) >> 39) & 0x1ff)
#define PDPT_INDEX(va)  (((va) >> 30) & 0x1ff)
#define PD_INDEX(va)    (((va) >> 21) & 0x1ff)
#define PT_INDEX(va)    (((va) >> 12) & 0x1ff)

/*
 * The low megabyte, which is not RAM the allocator may have.
 *
 * It is the PC's own past: the BIOS data area, the VGA window at 0xB8000,
 * the option ROMs. Mapped uncached and never executable, because the one
 * thing in it worth reaching is a text-mode screen on a machine with no
 * framebuffer - and page 0 is left out so a null dereference faults.
 *
 * The ARM board draws this line at 0x08000000 for the same reason and calls
 * it the device region. Here the devices are mostly behind port I/O rather
 * than addresses, which is why this range is a megabyte instead of a
 * gigabyte.
 */
#define LOW_BASE        PAGE_SIZE
#define LOW_END         0x100000UL

static uint64_t *kernel_pml4;
static uint64_t *kernel_pdpt;

static uint64_t *alloc_table(void)
{
    uint64_t *table = pmm_alloc_page();
    unsigned i;

    if (table == NULL) {
        panic("mmu: out of pages while building the page tables");
    }

    /* A page comes out of the allocator holding whatever the last owner
     * left. A stale present entry is a mapping nobody asked for. */
    for (i = 0; i < ENTRIES_PER_TABLE; i++) {
        table[i] = 0;
    }

    return table;
}

/* Invalidates one page's translation. */
static void invalidate(uintptr_t va)
{
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

/*
 * Replaces a 2 MB page with a table of 512 page entries covering the same
 * range with the same attributes. The mapping does not change; only its
 * granularity does.
 *
 * Needed because most of RAM is mapped in 2 MB pages, and a thread stack
 * allocated out of it cannot have a guard page punched into it while the
 * smallest thing describing that range is two megabytes.
 *
 * **PTE_PS has to come off, and bit 7 is why this is not a copy.** In a PD
 * entry bit 7 means "this is a 2 MB page"; in a PT entry the same bit is
 * PAT and selects a cache type. Carrying the attributes across without
 * clearing it would turn every one of the 512 new pages into a differently
 * cached one, silently.
 */
static uint64_t *split_2m(uint64_t *entry)
{
    uint64_t *table = alloc_table();
    uint64_t base = *entry & PTE_ADDR_MASK;
    uint64_t attrs = *entry & ~(PTE_ADDR_MASK | PTE_PS);
    unsigned i;

    for (i = 0; i < ENTRIES_PER_TABLE; i++) {
        table[i] = (base + (uint64_t)i * PAGE_SIZE) | attrs;
    }

    /*
     * No barrier between filling the table and publishing it, and that is
     * an architectural difference rather than an omission. AArch64 needs a
     * `dsb ishst` here because its stores may still be in a buffer when the
     * walker is pointed at them. x86-64's memory model orders stores
     * against each other, and the page walker is coherent with them, so the
     * table is visible by the time the entry describing it is.
     */
    *entry = (uint64_t)(uintptr_t)table | TABLE_ATTRS;

    /*
     * The old 2 MB entry may be cached in the TLB. `invlpg` on one address
     * inside it is defined to remove it, but reloading CR3 is the
     * unambiguous thing to do for a structural change and this happens
     * once per stack rather than in a loop.
     */
    __asm__ volatile("movq %%cr3, %%rax; movq %%rax, %%cr3" ::: "rax", "memory");

    return table;
}

/* The next level down, created on demand, splitting a large page if one is
 * in the way. */
static uint64_t *descend(uint64_t *table, unsigned index)
{
    uint64_t *next;

    if ((table[index] & PTE_P) == 0) {
        next = alloc_table();
        table[index] = (uint64_t)(uintptr_t)next | TABLE_ATTRS;
        return next;
    }

    if ((table[index] & PTE_PS) != 0) {
        /*
         * A 2 MB page in a PD. It cannot be a 1 GB page in a PDPT: nothing
         * here makes one, because a gigabyte is larger than the region any
         * of this describes and `map_blocks_2m` is the only thing that sets
         * PTE_PS at all.
         */
        return split_2m(&table[index]);
    }

    return (uint64_t *)(uintptr_t)(table[index] & PTE_ADDR_MASK);
}

/* PML4 down to the PD, which is the walk every mapping call shares. */
static uint64_t *reach_pd(uint64_t *root, uintptr_t va)
{
    uint64_t *pdpt = descend(root, (unsigned)PML4_INDEX(va));

    return descend(pdpt, (unsigned)PDPT_INDEX(va));
}

static void map_pages(uint64_t *root, uintptr_t va, uintptr_t pa, size_t count,
                      uint64_t attrs)
{
    size_t i;

    for (i = 0; i < count; i++) {
        uint64_t *pd = reach_pd(root, va);
        uint64_t *pt = descend(pd, (unsigned)PD_INDEX(va));

        pt[PT_INDEX(va)] = (pa & PTE_ADDR_MASK) | attrs;

        va += PAGE_SIZE;
        pa += PAGE_SIZE;
    }
}

static void map_blocks_2m(uint64_t *root, uintptr_t va, uintptr_t pa,
                          size_t count, uint64_t attrs)
{
    size_t i;

    for (i = 0; i < count; i++) {
        uint64_t *pd = reach_pd(root, va);

        pd[PD_INDEX(va)] = (pa & PTE_ADDR_MASK) | attrs | PTE_PS;

        va += BLOCK_2M;
        pa += BLOCK_2M;
    }
}

static uint64_t *page_entry(uint64_t *root, uintptr_t va)
{
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t *pt;

    if ((root[PML4_INDEX(va)] & PTE_P) == 0) {
        return NULL;
    }
    pdpt = (uint64_t *)(uintptr_t)(root[PML4_INDEX(va)] & PTE_ADDR_MASK);

    if ((pdpt[PDPT_INDEX(va)] & PTE_P) == 0
        || (pdpt[PDPT_INDEX(va)] & PTE_PS) != 0) {
        return NULL;
    }
    pd = (uint64_t *)(uintptr_t)(pdpt[PDPT_INDEX(va)] & PTE_ADDR_MASK);

    if ((pd[PD_INDEX(va)] & PTE_P) == 0 || (pd[PD_INDEX(va)] & PTE_PS) != 0) {
        return NULL;    /* a 2 MB page, or nothing at all */
    }
    pt = (uint64_t *)(uintptr_t)(pd[PD_INDEX(va)] & PTE_ADDR_MASK);

    return &pt[PT_INDEX(va)];
}

uint64_t *mmu_page_entry(uintptr_t va)
{
    return page_entry(kernel_pml4, va);
}

static void unmap_page(uint64_t *root, uintptr_t va)
{
    /* descend rather than page_entry, because this is the caller that has
     * to work on an address currently covered by a 2 MB page: that is
     * exactly what a guard page inside a freshly allocated stack is. */
    uint64_t *pd = reach_pd(root, va);
    uint64_t *pt = descend(pd, (unsigned)PD_INDEX(va));

    pt[PT_INDEX(va)] = 0;
    invalidate(va);
}

void mmu_unmap_page(uintptr_t va)
{
    unmap_page(kernel_pml4, va);
}

/*
 * Turns on the two bits the map depends on, then loads it.
 *
 * Much shorter than the ARM `enable()`, and the reason is that paging is
 * already on: long mode cannot be entered without it, so `start.S` built a
 * gigabyte of 2 MB identity mappings on the way in. Switching to the real
 * map is one write to CR3, and it is safe because both maps describe the
 * code doing the writing at the same address.
 *
 * The order is not: **EFER.NXE first**. Until it is set, bit 63 of an entry
 * is a reserved bit rather than no-execute, and loading a CR3 whose entries
 * have it set is a page fault on the next instruction with a reserved-bit
 * error code - which reads as a corrupt page table rather than as a missing
 * feature bit.
 */
static void enable(void)
{
    uint32_t lo, hi;
    uint32_t max, ebx;
    uint64_t cr4;

    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
    lo |= (1u << 11);                                   /* NXE */
    __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(0xC0000080u));

    /*
     * SMEP, where the processor has it: the substitute for AArch64's PXN.
     *
     * `mmu.h` says why it is needed - x86 has one execute-never bit for
     * both privilege levels, so a page a process may execute the kernel may
     * execute too. SMEP makes ring 0 fetching from a PTE_US page a fault,
     * which is the guarantee PXN gives per page, given once for the whole
     * machine.
     *
     * CPUID leaf 7 does not exist on every processor this could run on, so
     * the maximum leaf is asked for first. Reading leaf 7 on a CPU that
     * stops at 1 returns the highest leaf it does have, and the answer is
     * whatever that leaf's EBX happens to hold.
     */
    __asm__ volatile("cpuid" : "=a"(max) : "a"(0) : "ebx", "ecx", "edx");

    if (max >= 7) {
        __asm__ volatile("cpuid"
                         : "=b"(ebx)
                         : "a"(7), "c"(0)
                         : "edx");

        if (ebx & (1u << 7)) {
            __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
            __asm__ volatile("movq %0, %%cr4" :: "r"(cr4 | (1UL << 20)));
        }
    }

    /*
     * CR0.WP, and **without it every read-only mapping above is decoration**.
     *
     * This is the x86 rule with no AArch64 counterpart at all: out of reset,
     * a write from ring 0 to a page whose PTE_RW is clear *succeeds*. The
     * permission is enforced against ring 3 only. AP_RO_EL1 on ARM means
     * read-only at EL1 and there is no bit that switches that off.
     *
     * It was found the way these things are found. `mmu_init` narrowed .text
     * to MAP_TEXT, the entry read back as read-only and executable exactly
     * as intended, and a deliberate store into `__text_start` went through
     * without a fault - so the map was right and the machine was ignoring it.
     *
     * The reason the bit exists is that early Unix on the 386 wanted
     * copy-on-write pages the kernel could write through while a process
     * could not, and turning it off was cheaper than a second mapping. Every
     * kernel since sets it and brackets the few places that need it off.
     */
    {
        uint64_t cr0;

        __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
        __asm__ volatile("movq %0, %%cr0" :: "r"(cr0 | (1UL << 16)));
    }

    __asm__ volatile("movq %0, %%cr3" :: "r"((uint64_t)(uintptr_t)kernel_pml4)
                     : "memory");
}

/*
 * Does this processor have a PAT at all?
 *
 * CPUID.01H:EDX bit 16. Every processor since the Pentium III does; the
 * check exists so that the answer to "what if it does not" is uncached
 * rather than a write-combining mapping that means something else.
 */
static bool has_pat(void)
{
    uint32_t eax, ebx, ecx, edx;

    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1u), "c"(0u));

    return (edx & (1u << 16)) != 0;
}

/*
 * Slot 4 of the PAT becomes write-combining; slots 0 to 3 keep the meanings
 * they have out of reset.
 *
 * **That division is the whole design.** PCD and PWT alone select among the
 * first four, so every mapping written before this existed - and every one
 * written since that does not set `PTE_PAT` - names a slot this does not
 * touch and means exactly what it always meant. Only `MAP_FRAMEBUFFER`
 * reaches slot 4.
 *
 * Intel SDM volume 3 §11.12.4 asks for the caches and TLB to be flushed
 * around the write. Here it happens inside `mmu_init` before the new tables
 * are loaded and while the only mappings in existence are the boot
 * identity map, so there is nothing cached under a type that is about to
 * change meaning - but the sequence is written out anyway, because the next
 * person to move this call will not have that guarantee.
 */
static void pat_init(void)
{
    uint32_t low, high;
    uint64_t pat;

    if (!has_pat()) {
        return;
    }

    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(IA32_PAT));

    pat = ((uint64_t)high << 32) | low;
    pat &= ~((uint64_t)0xff << (PAT_SLOT_WC * 8));
    pat |= (uint64_t)PAT_TYPE_WC << (PAT_SLOT_WC * 8);

    __asm__ volatile("wbinvd" ::: "memory");
    __asm__ volatile("wrmsr" :: "c"(IA32_PAT),
                     "a"((uint32_t)pat), "d"((uint32_t)(pat >> 32)));
    __asm__ volatile("wbinvd" ::: "memory");
}

void mmu_init(void)
{
    uintptr_t fine_start;
    uintptr_t fine_end;
    uintptr_t ram_end;
    uintptr_t blocks_end;
    struct memrange ram;
    uintptr_t text_start   = (uintptr_t)__text_start;
    uintptr_t rodata_start = (uintptr_t)__rodata_start;
    uint64_t *guard;

    hal_ram_range(&ram);
    ram_end = ram.base + ram.size;

    /*
     * RAM is identity mapped, so it cannot reach into the region processes
     * are given. `mmu.h` explains why that region starts at 1 GB here and
     * what removes the limit.
     *
     * **The board has already capped its answer to fit**, in
     * `cap_to_what_can_be_mapped`, which is where the argument for capping
     * rather than refusing is written down. This is the assertion that it
     * did: a range past the line at this point is a board that reported
     * more than the architecture can describe, and mapping it would put a
     * process's first page inside the kernel's own tables.
     */
    if (ram_end > DEVICE_WINDOW_BASE) {
        panic("mmu: the board reported RAM the address space has no room for");
    }

    /*
     * The PAT first, before a single entry is written, so that
     * `MAP_FRAMEBUFFER` means write-combining from the first mapping that
     * uses it rather than from whenever this happened to run.
     */
    pat_init();

    kernel_pml4 = alloc_table();
    kernel_pdpt = alloc_table();
    kernel_pml4[0] = (uint64_t)(uintptr_t)kernel_pdpt | TABLE_ATTRS;

    /* The low megabyte, minus page 0. Uncached, never executed. */
    map_pages(kernel_pml4, LOW_BASE, LOW_BASE,
              (LOW_END - LOW_BASE) / PAGE_SIZE, MAP_DEVICE);

    /*
     * The bottom of RAM holds the kernel image, the page allocator's bitmap
     * and the boot stack, and is mapped a page at a time: per section
     * permissions and an unmapped guard page both need granularity finer
     * than a 2 MB page.
     *
     * How far up is decided by the image rather than by a constant, for the
     * reason `arch/aarch64/mmu.c` gives at length: the moment the image
     * grows past a fixed line, the guard page lands inside a 2 MB page and
     * the machine panics here about a stack guard, when what actually
     * happened is that something was added to the image.
     */
    /*
     * `__framebuffer_start` rather than `__image_end`, which is what the
     * ARM side uses and for the reason its comment gives: the framebuffer
     * above it is megabytes that only ever want to be writable, and mapping
     * those a page at a time is thousands of descriptors for nothing. The
     * fine region runs to the 2 MB boundary at or above the last thing that
     * needs page granularity, and grows by itself the next time the image
     * does.
     */
    fine_end = ((uintptr_t)__framebuffer_start + BLOCK_2M - 1)
               & ~(uintptr_t)(BLOCK_2M - 1);

    /*
     * **From the image, not from the RAM base**, and the two are not the
     * same address on a machine whose firmware fragments low memory.
     *
     * This read `ram.base` and worked for as long as the largest usable
     * region was the one the kernel had been loaded into - which is what
     * QEMU's `-kernel` gives, where RAM begins at 1 MB and the image is at
     * the bottom of it. Booted through GRUB under UEFI the same machine
     * reports its largest low region at 0x900000, because the firmware has
     * its own allocations below that, and the image sits at 0x100000
     * *outside* it. `fine_end` was 0x800000, the subtraction wrapped, and
     * the map asked for four quadrillion pages: `mmu: out of pages while
     * building the page tables`, at boot stage four, on a machine with no
     * serial port.
     *
     * The lower of the two is the right start either way. Where they are
     * the same address this is exactly what it was; where they differ, the
     * span between them is the firmware's and identity mapping it costs a
     * few page tables and keeps one rule instead of two.
     */
    fine_start = ram.base < LOW_END ? ram.base : LOW_END;

    map_pages(kernel_pml4, fine_start, fine_start,
              (fine_end - fine_start) / PAGE_SIZE, MAP_RW);

    /*
     * Everything above it is anonymous memory and gets 2 MB pages, which is
     * a couple of hundred entries instead of 130,000 and a couple of
     * hundred TLB entries instead of the same.
     *
     * **The tail is mapped in pages, and the ARM side does not do this.**
     * It divides the remainder by 2 MB and lets the division truncate,
     * because `virt` reports RAM that is a whole number of 2 MB pages. A PC
     * does not: this machine reports 0x1FEDF000 bytes from 0x100000, so
     * truncating leaves the last few hundred kilobytes unmapped while the
     * allocator, which reads the same range, hands those pages out happily.
     * The fault would arrive much later and somewhere else entirely.
     */
    blocks_end = fine_end + ((ram_end - fine_end) / BLOCK_2M) * BLOCK_2M;

    map_blocks_2m(kernel_pml4, fine_end, fine_end,
                  (blocks_end - fine_end) / BLOCK_2M, MAP_RW);

    if (blocks_end < ram_end) {
        map_pages(kernel_pml4, blocks_end, blocks_end,
                  (ram_end - blocks_end) / PAGE_SIZE, MAP_RW);
    }

    /*
     * Now narrow the two regions that should not be writable. Done as a
     * second pass over an already complete map, so there is one place that
     * decides what is mapped and a separate one that decides what may be
     * done with it.
     */
    map_pages(kernel_pml4, text_start, text_start,
              ((uintptr_t)__text_end - text_start) / PAGE_SIZE, MAP_TEXT);
    map_pages(kernel_pml4, rodata_start, rodata_start,
              ((uintptr_t)__rodata_end - rodata_start) / PAGE_SIZE, MAP_RO);

    /* And punch out both stack guards, so an overflow faults rather than
     * walking into whatever is below it. The second is the exception
     * stack's: `trap.c` puts #PF and #DF on it, and a handler that
     * overflows it should fault rather than corrupt the stack it was
     * called to report on. */
    guard = page_entry(kernel_pml4, (uintptr_t)__stack_guard);
    if (guard == NULL) {
        panic("mmu: the stack guard is not page mapped");
    }
    *guard = 0;

    guard = page_entry(kernel_pml4, (uintptr_t)__exception_guard);
    if (guard == NULL) {
        panic("mmu: the exception stack guard is not page mapped");
    }
    *guard = 0;

    enable();
}

/*
 * A bump allocator over the device window, because devices are mapped once
 * at boot and never unmapped. Anything cleverer would be a free list for a
 * dozen mappings that outlive the machine.
 */
static uintptr_t framebuffer_va;

static uintptr_t device_next = DEVICE_WINDOW_BASE;

/*
 * The page tables that are loaded right now, which before `mmu_init` are
 * the ones `start.S` built: one PML4, one PDPT, four page directories of
 * 2 MB entries covering the first four gigabytes.
 *
 * Reached through CR3 rather than through the assembler's symbols, because
 * every one of those tables is inside the identity map that describes
 * itself - so following the physical addresses in them is exactly following
 * the pointers.
 */
void mmu_boot_uncached(uintptr_t base, size_t bytes)
{
    uint64_t cr3;
    uintptr_t at;
    uintptr_t last;

    if (bytes == 0) {
        return;
    }

    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));

    last = base + bytes - 1;

    for (at = base & ~(BLOCK_2M - 1); at <= last; at += BLOCK_2M) {
        uint64_t *table = (uint64_t *)(uintptr_t)(cr3 & PTE_ADDR_MASK);
        uint64_t *entry;
        unsigned level;

        /* PML4, then PDPT, then the page directory. Three steps rather than
         * four: what covers this address is a 2 MB leaf, not a page. */
        for (level = 0; level < 2; level++) {
            unsigned index = (unsigned)((at >> (39 - level * 9)) & 511);

            if ((table[index] & PTE_P) == 0) {
                return;
            }

            table = (uint64_t *)(uintptr_t)(table[index] & PTE_ADDR_MASK);
        }

        entry = &table[(at >> 21) & 511];

        if ((*entry & PTE_P) == 0 || (*entry & PTE_PS) == 0) {
            return;                     /* not the map this expects */
        }

        *entry |= PTE_PCD | PTE_PWT;
        invalidate(at);
    }
}

/*
 * **Asked of the page table, not of CPUID.**
 *
 * This used to return `has_pat()`, which says the processor *could* do
 * write-combining and nothing about whether this mapping got it - so the
 * boot log said "write-combining" on every machine with a PAT, which is
 * every machine. That is the same failure as the pitch line that claimed
 * padding whichever it was: a fact reported as an intention.
 *
 * It matters here more than most places. A framebuffer mapped uncached
 * instead is correct and perhaps twenty times slower, and the difference is
 * invisible under emulation - QEMU's framebuffer is host memory and TCG
 * models no cache, so both run identically. The only way to know on the
 * machine is to read back what was written.
 */
bool mmu_write_combining(void)
{
    uint64_t *entry;

    if (!has_pat() || framebuffer_va == 0) {
        return false;
    }

    entry = mmu_page_entry(framebuffer_va);

    if (entry == NULL || (*entry & PTE_P) == 0) {
        return false;
    }

    /* Slot 4: the PAT bit set, and neither of the two below it. */
    return (*entry & PTE_PAT) != 0
        && (*entry & (PTE_PCD | PTE_PWT)) == 0;
}

/*
 * Does this entry describe the same memory type the kernel's framebuffer
 * mapping got?
 *
 * **Asked of one entry against another, rather than against a constant.**
 * The framebuffer is mapped twice - once by the kernel and once into the
 * compositor - and a memory type lives in the entry, not in the memory, so
 * the two can disagree. They did: the compositor's was write-back, which
 * against the firmware's MTRR for a PCIe framebuffer resolves to uncached,
 * and the desktop ran at one bus transaction per four bytes while the
 * console beside it was fine.
 *
 * A machine with no framebuffer entry to compare against answers yes. There
 * is nothing to be inconsistent with, and a warning about a screen that
 * does not exist is noise on the one boot where the log matters most.
 */
bool mmu_entry_matches_framebuffer(uint64_t entry)
{
    const uint64_t mask = PTE_PAT | PTE_PCD | PTE_PWT;
    uint64_t *fb;

    if (framebuffer_va == 0) {
        return true;
    }

    fb = mmu_page_entry(framebuffer_va);

    if (fb == NULL || (*fb & PTE_P) == 0) {
        return true;
    }

    return (entry & mask) == (*fb & mask);
}

uintptr_t mmu_map_framebuffer(uintptr_t pa, size_t bytes)
{
    uintptr_t offset = pa & PAGE_MASK;
    uintptr_t start = pa - offset;
    size_t pages = (offset + bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t at = device_next;

    if (!has_pat()) {
        return mmu_map_device(pa, bytes);
    }

    if (bytes == 0 || at + pages * PAGE_SIZE > DEVICE_WINDOW_END) {
        return 0;
    }

    map_pages(kernel_pml4, at, start, pages, MAP_FRAMEBUFFER);
    device_next = at + pages * PAGE_SIZE;
    framebuffer_va = at;

    __asm__ volatile("movq %%cr3, %%rax; movq %%rax, %%cr3" ::: "rax", "memory");

    return at + offset;
}

uintptr_t mmu_map_device(uintptr_t pa, size_t bytes)
{
    uintptr_t offset = pa & PAGE_MASK;
    uintptr_t start = pa - offset;
    size_t pages = (offset + bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t at = device_next;

    if (bytes == 0 || at + pages * PAGE_SIZE > DEVICE_WINDOW_END) {
        return 0;
    }

    map_pages(kernel_pml4, at, start, pages, MAP_DEVICE);
    device_next = at + pages * PAGE_SIZE;

    /*
     * Every mapping this makes is a page table this walk built after CR3
     * was loaded, so the TLB may hold "not present" for these addresses.
     * Reloading is the blunt answer and this happens a dozen times at boot.
     */
    __asm__ volatile("movq %%cr3, %%rax; movq %%rax, %%cr3" ::: "rax", "memory");

    return at + offset;
}

bool mmu_is_enabled(void)
{
    uint64_t cr0;

    /*
     * CR0.PG, which on x86-64 is necessarily set: long mode cannot be
     * entered without paging and cannot be left with it. So this answers
     * the same question the ARM version does and the answer is one the
     * architecture already guarantees - kept because the kernel asks it,
     * and honest about being a formality here.
     */
    __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));

    return (cr0 & (1UL << 31)) != 0;
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
 * by a test instead. `arch/aarch64/mmu.c` records what happened the time
 * they disagreed, and the number here is the same number for the same
 * reason.
 */
#define ADDRSPACE_MAX   32

struct addrspace {
    uint64_t *pml4;
    uint64_t *pdpt;
    bool      in_use;
};

/* A fixed pool, like everything else in the kernel. Running out is a NULL
 * from as_create rather than a table that grows. */
static struct addrspace spaces[ADDRSPACE_MAX];

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

/*
 * The address-space pool's lock. The AArch64 twin explains the shape: the
 * claim happens inside, and the page allocation outside, so this lock is
 * never held while the PMM's is taken.
 *
 * This board cannot start a second core - `hal/pc/cpu_on.c` has no local
 * APIC - so nothing here ever contends. It is written anyway, because the
 * two files are the same design against different tables and a lock present
 * in one and absent in the other is exactly the difference nobody notices
 * until the second board grows a second core.
 */
static struct spinlock spaces_lock = SPINLOCK("address spaces");

struct addrspace *as_create(void)
{
    unsigned long flags = spin_lock(&spaces_lock);
    unsigned i;

    for (i = 0; i < ADDRSPACE_MAX; i++) {
        unsigned e;

        if (spaces[i].in_use) {
            continue;
        }

        spaces[i].in_use = true;
        spaces[i].pml4 = NULL;
        spaces[i].pdpt = NULL;
        spin_unlock(&spaces_lock, flags);

        spaces[i].pml4 = pmm_alloc_page();
        if (spaces[i].pml4 == NULL) {
            unsigned long back = spin_lock(&spaces_lock);

            spaces[i].in_use = false;
            spin_unlock(&spaces_lock, back);
            return NULL;
        }

        spaces[i].pdpt = pmm_alloc_page();
        if (spaces[i].pdpt == NULL) {
            unsigned long back;

            pmm_free_page(spaces[i].pml4);
            spaces[i].pml4 = NULL;

            back = spin_lock(&spaces_lock);
            spaces[i].in_use = false;
            spin_unlock(&spaces_lock, back);
            return NULL;
        }

        /*
         * A copy of the level that holds the user slots, so the kernel is
         * mapped here too. Without it the instruction after as_switch would
         * have no translation, and the fault handler would have none either.
         *
         * **The PDPT is what gets copied, not the PML4**, and that is where
         * this differs from the ARM version. The user region starts at 2 GB,
         * so it divides PDPT slots - slot 2 upwards - while PML4 slot 0
         * covers the whole first 512 GB and is therefore shared ground. A
         * space that copied only the PML4 would point at the kernel's own
         * PDPT, and every user mapping it made would appear in every space
         * at once, including the kernel's.
         *
         * Below the PDPT everything is shared, which is what confines user
         * mappings to slots the kernel does not use.
         */
        for (e = 0; e < ENTRIES_PER_TABLE; e++) {
            spaces[i].pml4[e] = kernel_pml4[e];
            spaces[i].pdpt[e] = kernel_pdpt[e];
        }

        spaces[i].pml4[0] = (uint64_t)(uintptr_t)spaces[i].pdpt | TABLE_ATTRS;

        return &spaces[i];
    }

    spin_unlock(&spaces_lock, flags);
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
        uint64_t *pd = reach_pd(as->pml4, va);
        uint64_t *pt = descend(pd, (unsigned)PD_INDEX(va));

        pt[PT_INDEX(va)] = (pa & PTE_ADDR_MASK) | attrs;
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
        unmap_page(as->pml4, va);
        va += PAGE_SIZE;
    }

    return AS_OK;
}

uint64_t *as_page_entry(struct addrspace *as, uintptr_t va)
{
    return page_entry(as->pml4, va);
}

uintptr_t as_page_phys(struct addrspace *as, uintptr_t va)
{
    uint64_t *entry = page_entry(as->pml4, va);

    if (entry == NULL || (*entry & PTE_P) == 0) {
        return 0;
    }

    return (uintptr_t)(*entry & PTE_ADDR_MASK);
}

bool as_user_may(struct addrspace *as, uintptr_t va, bool need_write)
{
    uint64_t *entry = page_entry(as->pml4, va);

    if (entry == NULL || (*entry & PTE_P) == 0) {
        return false;           /* not mapped in this space */
    }

    /*
     * PTE_US is the whole of it, and it is one bit where AArch64 has a
     * two-bit field. What this cannot see is the *walk*: an intermediate
     * entry without PTE_US makes the page unreachable from ring 3 whatever
     * the leaf says. `mmu.h` explains why that cannot happen here - every
     * intermediate entry this file writes is permissive and the leaf
     * carries the policy - and this function is one of the reasons that
     * rule has to hold rather than merely usually hold.
     */
    if ((*entry & PTE_US) == 0) {
        return false;
    }

    return !need_write || (*entry & PTE_RW) != 0;
}

void as_switch(struct addrspace *as)
{
    uint64_t root = (uint64_t)(uintptr_t)
                    ((as != NULL) ? as->pml4 : kernel_pml4);

    /*
     * Writing CR3 flushes every non-global entry, which is all of them:
     * PTE_G is never set here and CR4.PGE is never turned on, precisely so
     * that this one instruction is the whole of a space switch.
     *
     * The x86 equivalent of an ASID is PCID, and it is what makes a switch
     * cheap by *not* flushing. Worth having when there are processes
     * switching often, and the same milestone as ARM's ASIDs.
     */
    __asm__ volatile("movq %0, %%cr3" :: "r"(root) : "memory");
}

void as_destroy(struct addrspace *as)
{
    unsigned pdpti;

    if (as == NULL || !as->in_use) {
        return;
    }

    /*
     * Only the slots this space owns. Everything below USER_VA_BASE is the
     * kernel's, borrowed rather than copied, and freeing it would hand the
     * kernel's own page tables back to the allocator.
     */
    for (pdpti = (unsigned)PDPT_INDEX(USER_VA_BASE);
         pdpti < ENTRIES_PER_TABLE; pdpti++) {
        uint64_t *pd;
        unsigned pdi;

        if ((as->pdpt[pdpti] & PTE_P) == 0
            || (as->pdpt[pdpti] & PTE_PS) != 0) {
            continue;
        }

        pd = (uint64_t *)(uintptr_t)(as->pdpt[pdpti] & PTE_ADDR_MASK);

        for (pdi = 0; pdi < ENTRIES_PER_TABLE; pdi++) {
            if ((pd[pdi] & PTE_P) != 0 && (pd[pdi] & PTE_PS) == 0) {
                pmm_free_page((void *)(uintptr_t)(pd[pdi] & PTE_ADDR_MASK));
            }
        }

        pmm_free_page(pd);
    }

    pmm_free_page(as->pdpt);
    pmm_free_page(as->pml4);

    as->pdpt = NULL;
    as->pml4 = NULL;
    as->in_use = false;
}

const char *mmu_describe(void)
{
    return "4 KB pages, four levels, .text read-only and CR0.WP set";
}
