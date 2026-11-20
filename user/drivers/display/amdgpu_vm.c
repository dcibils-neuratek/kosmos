/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A Southern Islands GPU's GART and programs' address spaces, as entries
 * and programs (`amdgpu_vm.h`, `docs/amdgpu.md` V2). Pure: no hardware.
 */

#include "amdgpu_vm.h"
#include "amdgpu_decode.h"

uint64_t amdgpu_pte(uint64_t address, uint64_t flags)
{
    return (address & AMDGPU_PTE_ADDR_MASK) | (flags & ~AMDGPU_PTE_ADDR_MASK);
}

/* A PDE is the table's MC address and VALID (`amdgpu_gmc.c` 113-114, and
 * radeon's `radeon_vm_update_pdes` writing R600_PTE_VALID). */
uint64_t amdgpu_pde(uint64_t page_table_mc)
{
    return (page_table_mc & AMDGPU_PTE_ADDR_MASK) | AMDGPU_PTE_VALID;
}

uint64_t amdgpu_pte_address(uint64_t entry)
{
    return entry & AMDGPU_PTE_ADDR_MASK;
}

static unsigned log2_of(uint32_t x)
{
    unsigned n = 0;

    while (x > 1u) {
        x >>= 1;
        n++;
    }
    return n;
}

bool amdgpu_vm_geometry(uint32_t gigabytes, struct amdgpu_vm_geometry *out)
{
    unsigned bits;

    if (gigabytes < 1u || gigabytes > 1024u || (gigabytes & (gigabytes - 1u)) != 0) {
        return false;
    }

    /* Pages covered by the PD and the PTs together: 30 - 12 + log2(GB). */
    bits = log2_of(gigabytes) + 18u;
    out->gigabytes = gigabytes;
    out->block_size = gigabytes <= 8u ? bits - 9u : (bits + 3u) / 2u;
    out->pt_entries = 1u << out->block_size;
    out->pd_entries = 1u << (bits - out->block_size);
    out->max_pfn = gigabytes << 18;
    return true;
}

bool amdgpu_vm_index(const struct amdgpu_vm_geometry *g, uint64_t va,
                     uint32_t *pde, uint32_t *pte)
{
    uint64_t pfn = va >> 12;

    if (pfn >= g->max_pfn) {
        return false;
    }

    *pde = (uint32_t)(pfn >> g->block_size);
    *pte = (uint32_t)(pfn & (g->pt_entries - 1u));
    return true;
}

bool amdgpu_vm_walk(const struct amdgpu_vm_geometry *g, amdgpu_read64 read, void *env,
                    uint64_t pd_mc, uint64_t va, uint64_t *entry, const char **why)
{
    uint32_t di, ti;
    uint64_t pde, pte;

    if (!amdgpu_vm_index(g, va, &di, &ti)) {
        *why = "outside the address space";
        return false;
    }

    pde = read(env, pd_mc + (uint64_t)di * 8u);
    if ((pde & AMDGPU_PTE_VALID) == 0) {
        *why = "no page table";
        return false;
    }

    pte = read(env, amdgpu_pte_address(pde) + (uint64_t)ti * 8u);
    if ((pte & AMDGPU_PTE_VALID) == 0) {
        *why = "no page";
        return false;
    }

    *entry = pte;
    *why = NULL;
    return true;
}

uint32_t amdgpu_vm_base_reg(unsigned vmid)
{
    if (vmid < 8u) {
        return AMDGPU_VM_CONTEXT0_PT_BASE + (vmid << 2);
    }
    return AMDGPU_VM_CONTEXT8_PT_BASE + ((vmid - 8u) << 2);
}

void amdgpu_gtt_place(uint64_t vram_start, uint64_t vram_end, uint64_t mc_mask,
                      uint64_t *gtt_size, uint64_t *gtt_start, uint64_t *gtt_end)
{
    /* gtt_base_align is nought on Southern Islands, so the masks fall out. */
    uint64_t size_af = mc_mask - vram_end;
    uint64_t size_bf = vram_start;

    if (size_bf > size_af) {
        if (*gtt_size > size_bf) {
            *gtt_size = size_bf;
        }
        *gtt_start = vram_start - *gtt_size;
    } else {
        if (*gtt_size > size_af) {
            *gtt_size = size_af;
        }
        *gtt_start = vram_end + 1u;
    }
    *gtt_end = *gtt_start + *gtt_size - 1u;
}

bool amdgpu_gart_enable(struct amdgpu_prog *p, const struct amdgpu_gart_cfg *cfg)
{
    const struct amdgpu_vm_geometry *g = cfg->vm;
    unsigned i;

    amdgpu_prog_clear(p);

    if (g == NULL || g->block_size < 9u || g->block_size > 24u
        || (cfg->table_mc & 0xfffu) != 0 || (cfg->dummy_page & 0xfffu) != 0
        || (cfg->gtt_start & 0xfffu) != 0 || ((cfg->gtt_end + 1u) & 0xfffu) != 0
        || cfg->gtt_end <= cfg->gtt_start) {
        return false;
    }

    /* The L1 TLB (`si.c` 4278-4284): 0xA << 7 is Linux's, undocumented. */
    amdgpu_prog_wr(p, AMDGPU_MC_VM_MX_L1_TLB_CNTL,
                   (0xau << 7) | AMDGPU_L1_TLB_ENABLE | AMDGPU_L1_FRAGMENT_PROCESSING
                   | AMDGPU_SYSTEM_ACCESS_NOT_IN_SYS | AMDGPU_ADVANCED_DRIVER_MODEL);

    /* The L2 (4286-4294). */
    amdgpu_prog_wr(p, AMDGPU_VM_L2_CNTL,
                   AMDGPU_L2_ENABLE | AMDGPU_L2_FRAGMENT_PROCESSING
                   | AMDGPU_L2_PTE_LRU_BY_WRITE | AMDGPU_L2_PDE0_LRU_BY_WRITE
                   | AMDGPU_L2_QUEUE_SIZE(7) | AMDGPU_L2_CONTEXT1_IDENTITY(1));
    amdgpu_prog_wr(p, AMDGPU_VM_L2_CNTL2, AMDGPU_L2_INVALIDATE_ALL_L1 | AMDGPU_L2_INVALIDATE_L2);
    amdgpu_prog_wr(p, AMDGPU_VM_L2_CNTL3,
                   AMDGPU_L2_BIGK_ASSOCIATIVITY | AMDGPU_L2_BANK_SELECT(4)
                   | AMDGPU_L2_BIGK_FRAGMENT_SIZE(4));

    /* Context 0, the GART (4296-4303). */
    amdgpu_prog_wr(p, AMDGPU_VM_CONTEXT0_PT_START, (uint32_t)(cfg->gtt_start >> 12));
    amdgpu_prog_wr(p, AMDGPU_VM_CONTEXT0_PT_END, (uint32_t)(cfg->gtt_end >> 12));
    amdgpu_prog_wr(p, AMDGPU_VM_CONTEXT0_PT_BASE, (uint32_t)(cfg->table_mc >> 12));
    amdgpu_prog_wr(p, AMDGPU_VM_CONTEXT0_FAULT_DEFAULT, (uint32_t)(cfg->dummy_page >> 12));
    amdgpu_prog_wr(p, AMDGPU_VM_CONTEXT0_CNTL2, 0);
    amdgpu_prog_wr(p, AMDGPU_VM_CONTEXT0_CNTL,
                   AMDGPU_CTX_ENABLE | AMDGPU_CTX_DEPTH(0) | AMDGPU_CTX_RANGE_FAULT_DEFAULT);

    /* Three registers both drivers clear without naming (4305-4307;
     * `gmc_v6_0.c` 512-514 as dwords 0x575-0x577). */
    amdgpu_prog_wr(p, 0x15d4u, 0);
    amdgpu_prog_wr(p, 0x15d8u, 0);
    amdgpu_prog_wr(p, 0x15dcu, 0);

    /* Contexts 1-15: the range, and a base that is valid until a program's
     * own is written - the GART's table, as `gmc_v6_0.c` 520-527 gives. */
    amdgpu_prog_wr(p, AMDGPU_VM_CONTEXT1_PT_START, 0);
    amdgpu_prog_wr(p, AMDGPU_VM_CONTEXT1_PT_END, g->max_pfn - 1u);
    for (i = 1; i < 16u; i++) {
        amdgpu_prog_wr(p, amdgpu_vm_base_reg(i), (uint32_t)(cfg->table_mc >> 12));
    }

    /* Their faults: to the dummy page, and an interrupt (`si.c` 4325-4342). */
    amdgpu_prog_wr(p, AMDGPU_VM_CONTEXT1_FAULT_DEFAULT, (uint32_t)(cfg->dummy_page >> 12));
    amdgpu_prog_wr(p, AMDGPU_VM_CONTEXT1_CNTL2, 4);
    amdgpu_prog_wr(p, AMDGPU_VM_CONTEXT1_CNTL,
                   AMDGPU_CTX_ENABLE | AMDGPU_CTX_DEPTH(1) | AMDGPU_CTX_BLOCK_SIZE(g->block_size - 9u)
                   | AMDGPU_CTX_RANGE_FAULT_INT | AMDGPU_CTX_RANGE_FAULT_DEFAULT
                   | AMDGPU_CTX_DUMMY_FAULT_INT | AMDGPU_CTX_DUMMY_FAULT_DEFAULT
                   | AMDGPU_CTX_PDE0_FAULT_INT | AMDGPU_CTX_PDE0_FAULT_DEFAULT
                   | AMDGPU_CTX_VALID_FAULT_INT | AMDGPU_CTX_VALID_FAULT_DEFAULT
                   | AMDGPU_CTX_READ_FAULT_INT | AMDGPU_CTX_READ_FAULT_DEFAULT
                   | AMDGPU_CTX_WRITE_FAULT_INT | AMDGPU_CTX_WRITE_FAULT_DEFAULT);

    /* `si_pcie_gart_tlb_flush` (4255-4262). */
    amdgpu_prog_wr(p, AMDGPU_HDP_MEM_FLUSH, 1);
    amdgpu_prog_wr(p, AMDGPU_VM_INVALIDATE_REQUEST, 1);

    return !p->overflow;
}

void amdgpu_sys_aperture(struct amdgpu_prog *p, uint64_t vram_start, uint64_t vram_end,
                         uint64_t default_page, uint32_t low, uint32_t high)
{
    uint32_t want_low = (uint32_t)(vram_start >> 12);
    uint32_t want_high = (uint32_t)(vram_end >> 12);

    if (low == want_low && high == want_high) {
        return;
    }

    amdgpu_prog_wr(p, AMDGPU_MC_VM_SYS_APER_LOW, want_low);
    amdgpu_prog_wr(p, AMDGPU_MC_VM_SYS_APER_HIGH, want_high);
    amdgpu_prog_wr(p, AMDGPU_MC_VM_SYS_APER_DEFAULT, (uint32_t)(default_page >> 12));
}

void amdgpu_vm_fault_decode(uint32_t status, uint32_t addr, struct amdgpu_vm_fault *out)
{
    out->address = (uint64_t)addr << 12;
    out->protections = status & 0xfu;
    out->client = (status >> 12) & 0xffu;
    out->write = ((status >> 24) & 1u) != 0;
    out->vmid = (status >> 25) & 0xfu;
}
