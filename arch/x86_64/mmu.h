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
#define PTE_PAT         (1UL << 7)      /* ...and the PAT selector, on a 4 KB
                                         * leaf. The same bit means two
                                         * things at two levels and the two
                                         * never meet. */
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
 * **A framebuffer is not a register file, and giving it `MAP_DEVICE` is
 * correct and slow.**
 *
 * Uncached means every store is its own bus transaction. That is exactly
 * right for a device register, where the write *is* the command and merging
 * two of them would be a bug that looks like flaky hardware. It is wrong
 * for eight megabytes of pixels a compositor rewrites sixty times a second.
 *
 * Write-combining is the type the architecture provides for this: stores
 * accumulate in a fill buffer and leave as whole cache lines, with no
 * ordering promised between them - which a framebuffer does not need,
 * because nothing reads it back and the only deadline is the next frame.
 *
 * Selected through the PAT rather than through PCD and PWT alone: with
 * `PTE_PAT` set and both of the others clear this names slot 4, which
 * `mmu_init` programs to write-combining. The four slots below it keep
 * their reset meanings, so every existing mapping in the system means what
 * it always meant.
 *
 * **Not merely an optimisation, and this is the part that was nearly
 * missed.** The early framebuffer is reached through `start.S`'s identity
 * map, whose 2 MB entries are plain present-and-writable - which is
 * *write-back cached*. Under QEMU that is invisible, because TCG models no
 * cache and every store lands at once. On a machine with a real one the
 * boot log would sit in cache and reach the panel when a line happened to
 * be evicted, which is the failure the early screen exists to prevent.
 */
#define MAP_FRAMEBUFFER (PTE_P | PTE_RW | PTE_PAT | PTE_NX)

/* IA32_PAT, and the slot this kernel reprograms. Intel SDM volume 3,
 * table 11-10 for the encodings and 11-12 for what selects which. */
#define IA32_PAT        0x277u
#define PAT_SLOT_WC     4u
#define PAT_TYPE_WC     0x01u

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

/*
 * **And the screen, which is a user mapping of the pages `MAP_FRAMEBUFFER`
 * already describes.**
 *
 * So it needs both halves: `PTE_US`, because the compositor is a process,
 * and `PTE_PAT`, because these are the *same physical pages* and a memory
 * type is a property of the mapping rather than of the memory. Two
 * mappings of one framebuffer can disagree about it, and for a long time
 * these two did.
 *
 * It was `MAP_USER_RW` until the first machine with a real cache ran it.
 * The comment above `MAP_FRAMEBUFFER` had the whole argument written out -
 * uncached is "wrong for eight megabytes of pixels a compositor rewrites
 * sixty times a second" - and the compositor was the one thing in the
 * system that did not get it.
 *
 * What write-back costs here is not a cache that helps. Against the
 * firmware's MTRR for a PCIe framebuffer the two combine to *uncached*,
 * which is one bus transaction per four bytes; a 1920x1080 composite is
 * two million of them. The console stayed quick throughout, because the
 * console draws through the kernel's mapping - so the machine looked like
 * it had a slow desktop rather than a wrong page table, and the boot log
 * agreed with it by reporting the mapping that was right.
 *
 * Invisible under QEMU by construction, for the reason `mmu.c` gives:
 * TCG models no cache, so both types run identically. That is what the
 * assertion below is for - the invariant is checkable even where the
 * consequence is not.
 */
#define MAP_USER_FB (PTE_P | PTE_RW | PTE_US | PTE_PAT | PTE_NX)

/*
 * The two mappings of the framebuffer must agree about its memory type.
 *
 * A static assertion rather than a test, because this is the exact mistake
 * it is protecting against: somebody changes one constant, every suite
 * passes, and the machine that shows it is on another desk. Only the
 * cache-type bits are compared - `PTE_US` differs on purpose, and that is
 * the whole reason there are two constants.
 */
_Static_assert((MAP_USER_FB     & (PTE_PAT | PTE_PCD | PTE_PWT))
            == (MAP_FRAMEBUFFER & (PTE_PAT | PTE_PCD | PTE_PWT)),
               "the compositor's framebuffer mapping must carry the "
               "kernel's memory type");

/*
 * Where a device's registers get mapped, and why they are not identity
 * mapped like everything else.
 *
 * **A PC puts its PCI windows above three gigabytes**, which on this
 * machine is inside the region processes are given - `USER_VA_BASE` is 1 GB
 * here, because 0x80000000 is the first address x86-64's default code model
 * cannot reach. So the one rule the ARM map keeps, that a printed address
 * is the address, cannot hold for devices: their physical addresses are in
 * user space.
 *
 * They get a window at the top of the kernel's own PDPT slot instead. RAM
 * is identity mapped from 1 MB up and this begins at 768 MB, so the two
 * cannot meet on any machine `mmu_init` will accept - and it panics rather
 * than let them, exactly as it does for the user region.
 *
 * The high-half split this header already promises removes the whole
 * question: with the kernel out of every process's address space there is
 * no user region to collide with and devices go back to being identity
 * mapped.
 */
#define DEVICE_WINDOW_BASE  0x30000000UL
#define DEVICE_WINDOW_END   USER_VA_BASE

/*
 * Maps `bytes` of device registers and answers where they landed.
 *
 * Uncached and never executable, a page at a time because a BAR is
 * kilobytes rather than megabytes. Zero when the window is full, which a
 * caller must check: a driver that writes to address zero is a null
 * dereference with a device's name on it.
 */
uintptr_t mmu_map_device(uintptr_t pa, size_t bytes);

/*
 * The same window, write-combining rather than uncached. For a linear
 * framebuffer and nothing else - see `MAP_FRAMEBUFFER` above for why a
 * device register must not be mapped this way.
 *
 * Falls back to `mmu_map_device` on a processor with no PAT, which is
 * every 486 and nothing since; the fallback is correct and slow rather
 * than absent.
 */
uintptr_t mmu_map_framebuffer(uintptr_t pa, size_t bytes);

/*
 * Physical RAM outside the region the allocator manages - a disk a loader
 * left in memory - mapped into the device window, cached and never
 * executable. `mmu_map_device` would map the same pages uncached, which is
 * right for registers and very slow for bytes. Zero when the window has no
 * room left.
 */
uintptr_t mmu_map_ram(uintptr_t pa, size_t bytes);

/*
 * Whether `mmu_map_framebuffer` gives write-combining or falls back.
 *
 * For the boot log, and it is the only way to know from outside: the two
 * paths return an address that works either way, and the difference is a
 * bit in a page table entry and a byte in a model-specific register.
 * `run_uefi.py` reads this line, because the speed it buys cannot be
 * measured under emulation and the *correctness* of it can be asserted.
 */
bool mmu_write_combining(void);

/* Whether an entry from any space carries the memory type the kernel's
 * framebuffer mapping has. For checking a second mapping of those same
 * pages against the first - see `mmu.c` for the bug that motivates it. */
bool mmu_entry_matches_framebuffer(uint64_t entry);

/*
 * Marks a range uncached in the page tables that are loaded *now*.
 *
 * One caller: the early framebuffer, which is reached through `start.S`'s
 * identity map before `mmu_init` builds anything. Those entries are plain
 * present-and-writable, which is write-back cached, and write-back is the
 * one memory type MMIO may not have.
 *
 * Uncached rather than write-combining because the early log is a few
 * kilobytes of text and because a 2 MB entry names its PAT slot with a
 * different bit than a 4 KB one - a second encoding to get right for no
 * measurable gain.
 */
void mmu_boot_uncached(uintptr_t base, size_t bytes);

/* Builds the identity map and loads it. Needs pmm_init first, because the
 * tables come out of the page allocator. */
void mmu_init(void);

/*
 * The same for a processor started after this one: the PAT, NX and SMEP, write
 * protection, and the kernel's tables. Every one of those is a per-core
 * register that core zero set only for itself.
 */
void mmu_enable_here(void);

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

/*
 * Where a process's own address space begins, and it is 1 GB here against
 * AArch64's 2 GB.
 *
 * **The code model is what decides it.** x86-64 addresses static data with
 * a sign-extended 32-bit displacement unless told otherwise, which reaches
 * -2GB to +2GB - and 0x80000000 is exactly the first address it cannot
 * express. An image linked there fails with `relocation truncated to fit`
 * on every reference to its own `.bss`. `-mcmodel=large` is the other
 * answer and costs a register-materialised address on every access; moving
 * the region is free.
 *
 * 1 GB is not an arbitrary retreat from 2. It is the boundary of the first
 * PDPT slot, so **the kernel gets slot 0 and a process gets 1 upward** -
 * which is the same division AArch64 makes one level up, where the kernel
 * holds L1 slots 0 and 1 and a process starts at 2.
 *
 * The cost is a ceiling: RAM is identity mapped, so a machine with more
 * than a gigabyte of it would have the kernel's own map reaching into the
 * process's slots. `mmu_init` panics rather than letting that happen
 * quietly. The high-half split this header already promises - the kernel
 * out of every process's address space entirely - is what removes both the
 * ceiling and the reason for the division.
 */
#define USER_VA_BASE    0x40000000UL
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

/*
 * The two questions `kernel/` actually asks about a mapping.
 *
 * It used to read the descriptor itself - `*entry & DESC_VALID` for the
 * first and `(*entry >> 6) & 3` for the second - which is the one piece of
 * architecture that hid from a search for register names. **It is also the
 * most dangerous piece**, because `process_may_read` and `process_may_write`
 * are the check on every pointer a process hands the kernel: on a machine
 * where bits 6 and 7 mean something else, that check does not crash, it
 * quietly answers wrongly.
 *
 * `as_page_phys` returns 0 for an address that is not mapped. Physical zero
 * is a real address and is deliberately never mapped into a user space - it
 * is where a null dereference has to fault - so it is free to mean "no".
 */
uintptr_t as_page_phys(struct addrspace *as, uintptr_t va);
bool as_user_may(struct addrspace *as, uintptr_t va, bool need_write);

#define AS_OK           0
#define AS_ERR_RANGE   (-1)     /* outside the user region */
#define AS_ERR_ALIGN   (-2)     /* not page aligned */
#define AS_ERR_NOMEM   (-3)


/* One line for the boot log, because `kernel/main.c` printed a string
 * literal about this architecture's hardware until there were two. */
const char *mmu_describe(void);

#endif /* ARCH_X86_64_MMU_H */
