/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DRIVERS_DISPLAY_AMDGPU_VM_H
#define KOSMOS_DRIVERS_DISPLAY_AMDGPU_VM_H

/*
 * **A Southern Islands GPU's memory as its engines see it** (`docs/amdgpu.md`
 * V2): the GART - VM context 0's flat table, which lets an engine reach
 * pages of system memory - and the address spaces of programs, contexts 1
 * to 15, each a two-level table (a page directory of PDEs, page tables of
 * PTEs) at a base the driver writes per VMID before a program's work runs.
 *
 * Pure: the entries, the geometry, where the GART goes, and the program
 * that turns it all on, computed here and held by
 * `tools/test_amdgpu_engine.c`; the driver writes them.
 *
 * Linux v6.12: radeon's `sid.h` 369-482 for the registers, `si.c`'s
 * `si_pcie_gart_enable` (4265-4350) for their values, amdgpu's
 * `gmc_v6_0.c` (`gmc_v6_0_gart_enable`, 457-545) for its order, the entry
 * bits from radeon's `radeon.h` 847-855 and amdgpu's `amdgpu_vm.h` 57-70.
 */

#include <stdbool.h>
#include <stdint.h>

#include "amdgpu_ih.h"

/*----------------------------------------------------------------------
 * The registers (`sid.h`; `gmc_6_0_d.h`'s dwords, held equal by the test).
 *--------------------------------------------------------------------*/

#define AMDGPU_VM_L2_CNTL2               0x1404u
#define AMDGPU_VM_L2_CNTL3               0x1408u
#define AMDGPU_VM_L2_STATUS              0x140cu
#define AMDGPU_VM_CONTEXT1_CNTL          0x1414u
#define AMDGPU_VM_CONTEXT0_CNTL2         0x1430u
#define AMDGPU_VM_CONTEXT1_CNTL2         0x1434u
#define AMDGPU_VM_CONTEXT8_PT_BASE       0x1438u   /* 8-15, a dword apart */
#define AMDGPU_VM_INVALIDATE_REQUEST     0x1478u   /* bit n: context n */
#define AMDGPU_VM_INVALIDATE_RESPONSE    0x147cu
#define AMDGPU_VM_CONTEXT1_FAULT_STATUS  0x14dcu
#define AMDGPU_VM_CONTEXT1_FAULT_ADDR    0x14fcu   /* the page, >> 12 */
#define AMDGPU_VM_CONTEXT0_FAULT_DEFAULT 0x1518u
#define AMDGPU_VM_CONTEXT1_FAULT_DEFAULT 0x151cu
#define AMDGPU_VM_CONTEXT0_PT_BASE       0x153cu   /* 0-7, a dword apart */
#define AMDGPU_VM_CONTEXT0_PT_START      0x155cu
#define AMDGPU_VM_CONTEXT1_PT_START      0x1560u
#define AMDGPU_VM_CONTEXT0_PT_END        0x157cu
#define AMDGPU_VM_CONTEXT1_PT_END        0x1580u
#define AMDGPU_MC_VM_SYS_APER_DEFAULT    0x203cu

/* MC_VM_MX_L1_TLB_CNTL (`sid.h` 474-482). */
#define AMDGPU_L1_TLB_ENABLE             (1u << 0)
#define AMDGPU_L1_FRAGMENT_PROCESSING    (1u << 1)
#define AMDGPU_SYSTEM_ACCESS_NOT_IN_SYS  (3u << 3)
#define AMDGPU_ADVANCED_DRIVER_MODEL     (1u << 6)

/* VM_L2_CNTL (370-377), _CNTL2 (379-380), _CNTL3 (386-389). */
#define AMDGPU_L2_ENABLE                 (1u << 0)
#define AMDGPU_L2_FRAGMENT_PROCESSING    (1u << 1)
#define AMDGPU_L2_PTE_LRU_BY_WRITE       (1u << 9)
#define AMDGPU_L2_PDE0_LRU_BY_WRITE      (1u << 10)
#define AMDGPU_L2_QUEUE_SIZE(x)          (((uint32_t)(x) & 7u) << 15)
#define AMDGPU_L2_CONTEXT1_IDENTITY(x)   (((uint32_t)(x) & 3u) << 19)
#define AMDGPU_L2_INVALIDATE_ALL_L1      (1u << 0)
#define AMDGPU_L2_INVALIDATE_L2          (1u << 1)
#define AMDGPU_L2_BANK_SELECT(x)         ((uint32_t)(x) << 0)
#define AMDGPU_L2_BIGK_FRAGMENT_SIZE(x)  ((uint32_t)(x) << 15)
#define AMDGPU_L2_BIGK_ASSOCIATIVITY     (1u << 20)

/* VM_CONTEXTn_CNTL (392-407). */
#define AMDGPU_CTX_ENABLE                (1u << 0)
#define AMDGPU_CTX_DEPTH(x)              (((uint32_t)(x) & 3u) << 1)
#define AMDGPU_CTX_RANGE_FAULT_INT       (1u << 3)
#define AMDGPU_CTX_RANGE_FAULT_DEFAULT   (1u << 4)
#define AMDGPU_CTX_DUMMY_FAULT_INT       (1u << 6)
#define AMDGPU_CTX_DUMMY_FAULT_DEFAULT   (1u << 7)
#define AMDGPU_CTX_PDE0_FAULT_INT        (1u << 9)
#define AMDGPU_CTX_PDE0_FAULT_DEFAULT    (1u << 10)
#define AMDGPU_CTX_VALID_FAULT_INT       (1u << 12)
#define AMDGPU_CTX_VALID_FAULT_DEFAULT   (1u << 13)
#define AMDGPU_CTX_READ_FAULT_INT        (1u << 15)
#define AMDGPU_CTX_READ_FAULT_DEFAULT    (1u << 16)
#define AMDGPU_CTX_WRITE_FAULT_INT       (1u << 18)
#define AMDGPU_CTX_WRITE_FAULT_DEFAULT   (1u << 19)
#define AMDGPU_CTX_BLOCK_SIZE(x)         (((uint32_t)(x) & 0xfu) << 24)

/*----------------------------------------------------------------------
 * Entries: 64 bits, the address in 39:12 (`gmc_v6_0_get_vm_pde`'s
 * BUG_ON(addr & 0xFFFFFF0000000FFF)), the flags below it.
 *--------------------------------------------------------------------*/

#define AMDGPU_PTE_VALID                 (1ull << 0)
#define AMDGPU_PTE_SYSTEM                (1ull << 1)   /* a bus address, not VRAM */
#define AMDGPU_PTE_SNOOPED               (1ull << 2)   /* the processor's cache is looked in */
#define AMDGPU_PTE_READABLE              (1ull << 5)
#define AMDGPU_PTE_WRITEABLE             (1ull << 6)
#define AMDGPU_PTE_FRAG(x)               (((uint64_t)(x) & 0x1fu) << 7)
#define AMDGPU_PTE_ADDR_MASK             0x000000fffffff000ull

/* A page of system memory, cached by the processor: what a fence page and
 * a program's buffers are (radeon's R600_PTE_GART_MASK plus snooping). */
#define AMDGPU_PTE_SYSTEM_RW  (AMDGPU_PTE_VALID | AMDGPU_PTE_SYSTEM | AMDGPU_PTE_SNOOPED \
                               | AMDGPU_PTE_READABLE | AMDGPU_PTE_WRITEABLE)

uint64_t amdgpu_pte(uint64_t address, uint64_t flags);
uint64_t amdgpu_pde(uint64_t page_table_mc);
uint64_t amdgpu_pte_address(uint64_t entry);

/*----------------------------------------------------------------------
 * A program's address space: its size, and the split between the page
 * directory and the tables, as radeon chooses it when nobody says
 * (`radeon_device.c` 1179-1190: "the PD is 4K in size up to 8GB address
 * space. Above that split equal between PD and PTs").
 *--------------------------------------------------------------------*/

struct amdgpu_vm_geometry {
    uint32_t gigabytes;             /* a power of two, 1 to 1024 (`radeon_device.c` 1155-1175) */
    uint32_t block_size;            /* log2 of a table's entries; VM_CONTEXT1_CNTL takes it - 9 */
    uint32_t pd_entries, pt_entries;
    uint32_t max_pfn;               /* pages: VM_CONTEXT1_PAGE_TABLE_END_ADDR is this - 1 */
};

bool amdgpu_vm_geometry(uint32_t gigabytes, struct amdgpu_vm_geometry *out);

/* Where `va` is: which PDE, and which PTE of the table it points to. */
bool amdgpu_vm_index(const struct amdgpu_vm_geometry *g, uint64_t va,
                     uint32_t *pde, uint32_t *pte);

/*
 * **Walked back**, as the GPU walks it: `read` answers the 64-bit entry at
 * an MC address (the driver's reads through BAR 0, the test's from an
 * array); false for an address the tables do not map, with `*why` "no
 * page table" or "no page".
 */
typedef uint64_t (*amdgpu_read64)(void *env, uint64_t mc);

bool amdgpu_vm_walk(const struct amdgpu_vm_geometry *g, amdgpu_read64 read, void *env,
                    uint64_t pd_mc, uint64_t va, uint64_t *entry, const char **why);

/* VM_CONTEXTn_PAGE_TABLE_BASE_ADDR's register for a VMID (`si.c` 4317-4323). */
uint32_t amdgpu_vm_base_reg(unsigned vmid);

/*----------------------------------------------------------------------
 * The GART: context 0, flat (depth 0), mapping [gtt_start, gtt_end] page
 * for page from a table in VRAM.
 *--------------------------------------------------------------------*/

/*
 * `radeon_gtt_location` (`radeon_device.c` 603-625): before VRAM when there
 * is more room below it than above, else after, in an MC space of
 * `mc_mask`; the size cut to what fits. Southern Islands' alignment is
 * nought (`si_vram_gtt_location`, `si.c` 4180) and its mask 40 bits.
 */
void amdgpu_gtt_place(uint64_t vram_start, uint64_t vram_end, uint64_t mc_mask,
                      uint64_t *gtt_size, uint64_t *gtt_start, uint64_t *gtt_end);

struct amdgpu_gart_cfg {
    uint64_t table_mc;              /* the flat table: (gtt_end - gtt_start + 1) / 4096 entries */
    uint64_t gtt_start, gtt_end;
    uint64_t dummy_page;            /* where a faulting access goes: a bus address */
    const struct amdgpu_vm_geometry *vm;    /* contexts 1-15 */
};

/*
 * `si_pcie_gart_enable`'s values in `gmc_v6_0_gart_enable`'s order: the L1
 * TLB, the L2, context 0 on the GART's table, the three undocumented
 * registers Linux clears, contexts 1-15 given the GART's table as a base
 * that is at least valid, their faults reported by interrupt and sent to
 * the dummy page, and the TLB flushed (`si_pcie_gart_tlb_flush`, 4255-4262).
 */
bool amdgpu_gart_enable(struct amdgpu_prog *p, const struct amdgpu_gart_cfg *cfg);

/*
 * The system aperture as `si_mc_program` sets it (4146-4150): VRAM, so an
 * engine's access to a VRAM address is never translated under
 * NOT_IN_SYS. Only what is different from `low`, `high` as read is
 * written; nothing when the firmware left it so.
 */
void amdgpu_sys_aperture(struct amdgpu_prog *p, uint64_t vram_start, uint64_t vram_end,
                         uint64_t default_page, uint32_t low, uint32_t high);

/*----------------------------------------------------------------------
 * A fault (`si_vm_decode_fault`, `si.c` 4801; fields `sid.h` 422-435).
 *--------------------------------------------------------------------*/

struct amdgpu_vm_fault {
    uint64_t address;               /* VM_CONTEXT1_PROTECTION_FAULT_ADDR << 12 */
    uint32_t protections;           /* 3:0 */
    uint32_t client;                /* 19:12 */
    bool     write;                 /* 24 */
    uint32_t vmid;                  /* 28:25 */
};

void amdgpu_vm_fault_decode(uint32_t status, uint32_t addr, struct amdgpu_vm_fault *out);

#endif /* KOSMOS_DRIVERS_DISPLAY_AMDGPU_VM_H */
