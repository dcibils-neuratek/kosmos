/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The AMD driver's engines held on the Mac (`docs/amdgpu.md` V1-V4): the
 * interrupt ring, the GART and programs' address spaces, the RLC and the
 * CP brought up, and the packets their rings take - `amdgpu_ih.c`,
 * `amdgpu_vm.c`, `amdgpu_cp.c` and the DMA builders in `amdgpu_decode.c`.
 *
 * Three kinds of check. **Values**: each register write and packet word
 * built here from the bit numbers Linux v6.12's radeon `sid.h` gives, and
 * compared. **Order**: a program's writes in the order amdgpu's
 * `si_ih.c`, `gmc_v6_0.c` and `gfx_v6_0.c` make them. **Offsets**: with
 * the downloaded headers named on the command line, every byte offset this
 * driver uses is read back from amdgpu's own `*_d.h` - which numbers
 * registers in dwords, from AMD's register database, independently of
 * radeon's - and held equal times four. And with the firmware directory
 * named too, the four files the CP and RLC need are parsed as the driver
 * will parse them.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O1 -o build/host/test_amdgpu_engine \
 *      tools/test_amdgpu_engine.c user/drivers/display/amdgpu_ih.c \
 *      user/drivers/display/amdgpu_vm.c user/drivers/display/amdgpu_cp.c \
 *      user/drivers/display/amdgpu_decode.c
 *   build/host/test_amdgpu_engine build/downloads/linux-amd build/downloads/radeon-firmware
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user/drivers/display/amdgpu_cp.h"
#include "../user/drivers/display/amdgpu_decode.h"
#include "../user/drivers/display/amdgpu_ih.h"
#include "../user/drivers/display/amdgpu_vm.h"

static int checks, failed, skipped;

static void check(int ok, const char *what)
{
    checks++;

    if (!ok) {
        failed++;
        printf("FAIL: %s\n", what);
    }
}

/* The index of the first write to `reg` at or after `from`, or -1. */
static int find(const struct amdgpu_prog *p, unsigned from, uint32_t reg, uint32_t value, int any)
{
    unsigned i;

    for (i = from; i < p->n; i++) {
        if (p->w[i].reg == AMDGPU_WR_POLL) {
            if (reg == AMDGPU_WR_POLL && (any || p->w[i].value == value)) {
                return (int)i;
            }
            i++;
            continue;
        }
        if (p->w[i].reg == reg && (any || p->w[i].value == value)) {
            return (int)i;
        }
    }
    return -1;
}

/* Writes `reg` = `value` somewhere after `*at`, and moves `*at` past it. */
static int then(const struct amdgpu_prog *p, int *at, uint32_t reg, uint32_t value)
{
    int i;

    if (*at < 0) {
        return 0;
    }
    i = find(p, (unsigned)*at, reg, value, 0);
    *at = i < 0 ? -1 : i + 1;
    return i >= 0;
}

/*----------------------------------------------------------------------
 * Offsets against amdgpu's dword headers.
 *--------------------------------------------------------------------*/

static char *slurp(const char *dir, const char *name, size_t *len)
{
    char path[1024];
    FILE *f;
    char *b;
    long n;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1u);
    if (b == NULL || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(b);
        return NULL;
    }
    fclose(f);
    b[n] = 0;
    if (len != NULL) {
        *len = (size_t)n;
    }
    return b;
}

/* `#define <name> <value>` in a header's text, or -1. */
static long define_of(const char *text, const char *name)
{
    size_t nl = strlen(name);
    const char *s = text;

    while ((s = strstr(s, "#define ")) != NULL) {
        const char *q = s + 8;

        s = q;
        while (*q == ' ' || *q == '\t') {
            q++;
        }
        if (strncmp(q, name, nl) == 0 && (q[nl] == ' ' || q[nl] == '\t')) {
            return strtol(q + nl, NULL, 0);
        }
    }
    return -1;
}

struct offset { const char *file, *name; uint32_t bytes; };

static const struct offset offsets[] = {
    { "asic_oss_oss_1_0_d.h", "mmIH_RB_CNTL", AMDGPU_IH_RB_CNTL },
    { "asic_oss_oss_1_0_d.h", "mmIH_RB_BASE", AMDGPU_IH_RB_BASE },
    { "asic_oss_oss_1_0_d.h", "mmIH_RB_RPTR", AMDGPU_IH_RB_RPTR },
    { "asic_oss_oss_1_0_d.h", "mmIH_RB_WPTR", AMDGPU_IH_RB_WPTR },
    { "asic_oss_oss_1_0_d.h", "mmIH_RB_WPTR_ADDR_HI", AMDGPU_IH_RB_WPTR_ADDR_HI },
    { "asic_oss_oss_1_0_d.h", "mmIH_RB_WPTR_ADDR_LO", AMDGPU_IH_RB_WPTR_ADDR_LO },
    { "asic_oss_oss_1_0_d.h", "mmIH_CNTL", AMDGPU_IH_CNTL },
    { "asic_oss_oss_1_0_d.h", "mmSRBM_INT_CNTL", AMDGPU_SRBM_INT_CNTL },
    { "asic_bif_bif_3_0_d.h", "mmINTERRUPT_CNTL", AMDGPU_INTERRUPT_CNTL },
    { "asic_bif_bif_3_0_d.h", "mmINTERRUPT_CNTL2", AMDGPU_INTERRUPT_CNTL2 },
    { "asic_bif_bif_3_0_d.h", "mmHDP_MEM_COHERENCY_FLUSH_CNTL", AMDGPU_HDP_MEM_FLUSH },
    { "asic_dce_dce_6_0_d.h", "mmINT_MASK", AMDGPU_INT_MASK },
    { "asic_dce_dce_6_0_d.h", "mmGRPH_INTERRUPT_CONTROL", AMDGPU_GRPH_INT_CONTROL },
    { "asic_gmc_gmc_6_0_d.h", "mmVM_L2_CNTL", AMDGPU_VM_L2_CNTL },
    { "asic_gmc_gmc_6_0_d.h", "mmVM_L2_CNTL2", AMDGPU_VM_L2_CNTL2 },
    { "asic_gmc_gmc_6_0_d.h", "mmVM_L2_CNTL3", AMDGPU_VM_L2_CNTL3 },
    { "asic_gmc_gmc_6_0_d.h", "mmVM_CONTEXT0_CNTL", AMDGPU_VM_CONTEXT0_CNTL },
    { "asic_gmc_gmc_6_0_d.h", "mmVM_CONTEXT1_CNTL", AMDGPU_VM_CONTEXT1_CNTL },
    { "asic_gmc_gmc_6_0_d.h", "mmVM_CONTEXT0_CNTL2", AMDGPU_VM_CONTEXT0_CNTL2 },
    { "asic_gmc_gmc_6_0_d.h", "mmVM_CONTEXT1_CNTL2", AMDGPU_VM_CONTEXT1_CNTL2 },
    { "asic_gmc_gmc_6_0_d.h", "mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR", AMDGPU_VM_CONTEXT0_PT_BASE },
    { "asic_gmc_gmc_6_0_d.h", "mmVM_CONTEXT7_PAGE_TABLE_BASE_ADDR", AMDGPU_VM_CONTEXT0_PT_BASE + 28u },
    { "asic_gmc_gmc_6_0_d.h", "mmVM_CONTEXT8_PAGE_TABLE_BASE_ADDR", AMDGPU_VM_CONTEXT8_PT_BASE },
    { "asic_gmc_gmc_6_0_d.h", "mmVM_CONTEXT15_PAGE_TABLE_BASE_ADDR", AMDGPU_VM_CONTEXT8_PT_BASE + 28u },
    { "asic_gmc_gmc_6_0_d.h", "mmVM_CONTEXT0_PAGE_TABLE_START_ADDR", AMDGPU_VM_CONTEXT0_PT_START },
    { "asic_gmc_gmc_6_0_d.h", "mmVM_CONTEXT1_PAGE_TABLE_START_ADDR", AMDGPU_VM_CONTEXT1_PT_START },
    { "asic_gmc_gmc_6_0_d.h", "mmVM_CONTEXT0_PAGE_TABLE_END_ADDR", AMDGPU_VM_CONTEXT0_PT_END },
    { "asic_gmc_gmc_6_0_d.h", "mmVM_CONTEXT1_PAGE_TABLE_END_ADDR", AMDGPU_VM_CONTEXT1_PT_END },
    { "asic_gmc_gmc_6_0_d.h", "mmVM_CONTEXT0_PROTECTION_FAULT_DEFAULT_ADDR", AMDGPU_VM_CONTEXT0_FAULT_DEFAULT },
    { "asic_gmc_gmc_6_0_d.h", "mmVM_CONTEXT1_PROTECTION_FAULT_DEFAULT_ADDR", AMDGPU_VM_CONTEXT1_FAULT_DEFAULT },
    { "asic_gmc_gmc_6_0_d.h", "mmVM_CONTEXT1_PROTECTION_FAULT_ADDR", AMDGPU_VM_CONTEXT1_FAULT_ADDR },
    { "asic_gmc_gmc_6_0_d.h", "mmVM_CONTEXT1_PROTECTION_FAULT_STATUS", AMDGPU_VM_CONTEXT1_FAULT_STATUS },
    { "asic_gmc_gmc_6_0_d.h", "mmVM_INVALIDATE_REQUEST", AMDGPU_VM_INVALIDATE_REQUEST },
    { "asic_gmc_gmc_6_0_d.h", "mmVM_INVALIDATE_RESPONSE", AMDGPU_VM_INVALIDATE_RESPONSE },
    { "asic_gmc_gmc_6_0_d.h", "mmMC_VM_MX_L1_TLB_CNTL", AMDGPU_MC_VM_MX_L1_TLB_CNTL },
    { "asic_gmc_gmc_6_0_d.h", "mmMC_VM_SYSTEM_APERTURE_LOW_ADDR", AMDGPU_MC_VM_SYS_APER_LOW },
    { "asic_gmc_gmc_6_0_d.h", "mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR", AMDGPU_MC_VM_SYS_APER_HIGH },
    { "asic_gmc_gmc_6_0_d.h", "mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR", AMDGPU_MC_VM_SYS_APER_DEFAULT },
    { "asic_gca_gfx_6_0_d.h", "mmGRBM_SOFT_RESET", AMDGPU_GRBM_SOFT_RESET },
    { "asic_gca_gfx_6_0_d.h", "mmGRBM_INT_CNTL", AMDGPU_GRBM_INT_CNTL },
    { "asic_gca_gfx_6_0_d.h", "mmGRBM_GFX_INDEX", AMDGPU_GRBM_GFX_INDEX },
    { "asic_gca_gfx_6_0_d.h", "mmSCRATCH_REG0", AMDGPU_SCRATCH_REG0 },
    { "asic_gca_gfx_6_0_d.h", "mmSCRATCH_UMSK", AMDGPU_SCRATCH_UMSK },
    { "asic_gca_gfx_6_0_d.h", "mmSCRATCH_ADDR", AMDGPU_SCRATCH_ADDR },
    { "asic_gca_gfx_6_0_d.h", "mmCP_SEM_WAIT_TIMER", AMDGPU_CP_SEM_WAIT_TIMER },
    { "asic_gca_gfx_6_0_d.h", "mmCP_SEM_INCOMPLETE_TIMER_CNTL", AMDGPU_CP_SEM_INCOMPLETE },
    { "asic_gca_gfx_6_0_d.h", "mmCP_COHER_CNTL2", AMDGPU_CP_COHER_CNTL2 },
    { "asic_gca_gfx_6_0_d.h", "mmCP_ME_CNTL", AMDGPU_CP_ME_CNTL },
    { "asic_gca_gfx_6_0_d.h", "mmCP_RB_WPTR_DELAY", AMDGPU_CP_RB_WPTR_DELAY },
    { "asic_gca_gfx_6_0_d.h", "mmCP_RB0_BASE", AMDGPU_CP_RB0_BASE },
    { "asic_gca_gfx_6_0_d.h", "mmCP_RB0_CNTL", AMDGPU_CP_RB0_CNTL },
    { "asic_gca_gfx_6_0_d.h", "mmCP_RB0_RPTR_ADDR", AMDGPU_CP_RB0_RPTR_ADDR },
    { "asic_gca_gfx_6_0_d.h", "mmCP_RB0_RPTR_ADDR_HI", AMDGPU_CP_RB0_RPTR_ADDR_HI },
    { "asic_gca_gfx_6_0_d.h", "mmCP_RB0_WPTR", AMDGPU_CP_RB0_WPTR },
    { "asic_gca_gfx_6_0_d.h", "mmCP_PFP_UCODE_ADDR", AMDGPU_CP_PFP_UCODE_ADDR },
    { "asic_gca_gfx_6_0_d.h", "mmCP_PFP_UCODE_DATA", AMDGPU_CP_PFP_UCODE_DATA },
    { "asic_gca_gfx_6_0_d.h", "mmCP_ME_RAM_RADDR", AMDGPU_CP_ME_RAM_RADDR },
    { "asic_gca_gfx_6_0_d.h", "mmCP_ME_RAM_WADDR", AMDGPU_CP_ME_RAM_WADDR },
    { "asic_gca_gfx_6_0_d.h", "mmCP_ME_RAM_DATA", AMDGPU_CP_ME_RAM_DATA },
    { "asic_gca_gfx_6_0_d.h", "mmCP_CE_UCODE_ADDR", AMDGPU_CP_CE_UCODE_ADDR },
    { "asic_gca_gfx_6_0_d.h", "mmCP_CE_UCODE_DATA", AMDGPU_CP_CE_UCODE_DATA },
    { "asic_gca_gfx_6_0_d.h", "mmCP_INT_CNTL_RING0", AMDGPU_CP_INT_CNTL_RING0 },
    { "asic_gca_gfx_6_0_d.h", "mmCP_INT_CNTL_RING1", AMDGPU_CP_INT_CNTL_RING1 },
    { "asic_gca_gfx_6_0_d.h", "mmCP_INT_CNTL_RING2", AMDGPU_CP_INT_CNTL_RING2 },
    { "asic_gca_gfx_6_0_d.h", "mmCP_DEBUG", AMDGPU_CP_DEBUG },
    { "asic_gca_gfx_6_0_d.h", "mmDB_DEPTH_INFO", AMDGPU_DB_DEPTH_INFO },
    { "asic_gca_gfx_6_0_d.h", "mmSPI_LB_CU_MASK", AMDGPU_SPI_LB_CU_MASK },
    { "asic_gca_gfx_6_0_d.h", "mmRLC_CNTL", AMDGPU_RLC_CNTL },
    { "asic_gca_gfx_6_0_d.h", "mmRLC_RL_BASE", AMDGPU_RLC_RL_BASE },
    { "asic_gca_gfx_6_0_d.h", "mmRLC_RL_SIZE", AMDGPU_RLC_RL_SIZE },
    { "asic_gca_gfx_6_0_d.h", "mmRLC_LB_CNTL", AMDGPU_RLC_LB_CNTL },
    { "asic_gca_gfx_6_0_d.h", "mmRLC_SAVE_AND_RESTORE_BASE", AMDGPU_RLC_SAVE_RESTORE_BASE },
    { "asic_gca_gfx_6_0_d.h", "mmRLC_LB_CNTR_MAX", AMDGPU_RLC_LB_CNTR_MAX },
    { "asic_gca_gfx_6_0_d.h", "mmRLC_LB_CNTR_INIT", AMDGPU_RLC_LB_CNTR_INIT },
    { "asic_gca_gfx_6_0_d.h", "mmRLC_CLEAR_STATE_RESTORE_BASE", AMDGPU_RLC_CLEAR_STATE_BASE },
    { "asic_gca_gfx_6_0_d.h", "mmRLC_UCODE_ADDR", AMDGPU_RLC_UCODE_ADDR },
    { "asic_gca_gfx_6_0_d.h", "mmRLC_UCODE_DATA", AMDGPU_RLC_UCODE_DATA },
    { "asic_gca_gfx_6_0_d.h", "mmRLC_MC_CNTL", AMDGPU_RLC_MC_CNTL },
    { "asic_gca_gfx_6_0_d.h", "mmRLC_UCODE_CNTL", AMDGPU_RLC_UCODE_CNTL },
    { "asic_gca_gfx_6_0_d.h", "mmRLC_STAT", AMDGPU_RLC_STAT },
    { "asic_gca_gfx_6_0_d.h", "mmRLC_LB_INIT_CU_MASK", AMDGPU_RLC_LB_INIT_CU_MASK },
    { "asic_gca_gfx_6_0_d.h", "mmRLC_SERDES_MASTER_BUSY_0", AMDGPU_RLC_SERDES_BUSY_0 },
    { "asic_gca_gfx_6_0_d.h", "mmRLC_SERDES_MASTER_BUSY_1", AMDGPU_RLC_SERDES_BUSY_1 },
    /* amdgpu's `sid.h` names the DMA engine's in dwords too. */
    { "drivers_gpu_drm_amd_amdgpu_sid.h", "DMA_CNTL", AMDGPU_DMA_CNTL },
    { "drivers_gpu_drm_amd_amdgpu_sid.h", "VM_INVALIDATE_REQUEST", AMDGPU_VM_INVALIDATE_REQUEST },
};

static void check_offsets(const char *dir)
{
    char what[160];
    unsigned i;

    for (i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
        char *text = slurp(dir, offsets[i].file, NULL);
        long dw;

        if (text == NULL) {
            snprintf(what, sizeof what, "%s is in %s", offsets[i].name, dir);
            check(0, what);
            continue;
        }
        dw = define_of(text, offsets[i].name);
        snprintf(what, sizeof what, "%s: 0x%x bytes is dword 0x%lx in %s",
                 offsets[i].name, offsets[i].bytes, dw, offsets[i].file);
        check(dw >= 0 && (uint32_t)dw * 4u == offsets[i].bytes, what);
        free(text);
    }
}

/*----------------------------------------------------------------------
 * V1: the IH.
 *--------------------------------------------------------------------*/

static void check_ih(void)
{
    struct amdgpu_prog p;
    struct amdgpu_ih_cfg c;
    struct amdgpu_iv iv;
    uint32_t dw[4], rb;
    bool ovf;
    int at;

    memset(&c, 0, sizeof c);
    c.ring_mc = 0xf400100000ull;
    c.ring_bytes = AMDGPU_IH_RING_BYTES;
    c.wptr_mc = 0xf400110000ull;
    c.dummy_mc = 0xf400111000ull;
    c.in_vram = true;
    c.msi = true;
    c.crtcs = 6;
    c.ih_rb_cntl = AMDGPU_IH_RB_ENABLE | 0x1cu;
    c.ih_cntl = AMDGPU_IH_ENABLE_INTR;
    c.interrupt_cntl = AMDGPU_IH_DUMMY_RD_OVERRIDE | (1u << 1);
    c.cp_int_cntl_ring0 = 0xffffffffu;
    c.dma_cntl[0] = AMDGPU_DMA_TRAP_ENABLE | AMDGPU_CTXEMPTY_INT;
    c.dma_cntl[1] = AMDGPU_DMA_TRAP_ENABLE;

    check(amdgpu_ih_rb_size(0x10000) == (14u << 1), "IH: a 64 KB ring is 2^14 dwords, << 1");
    check(amdgpu_ih_setup(&p, &c), "IH: the Mac Pro's configuration is one");
    check(!p.overflow && p.n < AMDGPU_PROG_MAX, "IH: the program fits");

    /* si_ih_irq_init's order. */
    check(p.w[0].reg == AMDGPU_IH_RB_CNTL && (p.w[0].value & AMDGPU_IH_RB_ENABLE) == 0,
          "IH: the ring is disabled first");
    at = 0;
    check(then(&p, &at, AMDGPU_IH_CNTL, 0), "IH: then IH_CNTL with interrupts off");
    check(then(&p, &at, AMDGPU_INTERRUPT_CNTL2, (uint32_t)(c.dummy_mc >> 8)), "IH: the dummy read's page, >> 8");
    check(then(&p, &at, AMDGPU_INTERRUPT_CNTL, (1u << 1) | AMDGPU_IH_REQ_NONSNOOP_EN),
          "IH: INTERRUPT_CNTL: no DUMMY_RD_OVERRIDE, NONSNOOP for a ring in VRAM, the rest kept");
    check(then(&p, &at, AMDGPU_IH_RB_BASE, (uint32_t)(c.ring_mc >> 8)), "IH: the ring's base, >> 8");
    check(then(&p, &at, AMDGPU_IH_RB_WPTR_ADDR_LO, (uint32_t)c.wptr_mc), "IH: write-back low");
    check(then(&p, &at, AMDGPU_IH_RB_WPTR_ADDR_HI, 0xf4u), "IH: write-back high, 8 bits");
    rb = (1u << 16) | (1u << 31) | 0x1cu;
    check(then(&p, &at, AMDGPU_IH_RB_CNTL, rb), "IH: RB_CNTL = OVERFLOW_ENABLE | OVERFLOW_CLEAR | size, no write-back");
    check(then(&p, &at, AMDGPU_IH_RB_RPTR, 0) && then(&p, &at, AMDGPU_IH_RB_WPTR, 0), "IH: the pointers nought");
    check(then(&p, &at, AMDGPU_IH_CNTL, (0x10u << 15) | (0x10u << 20) | (1u << 4)),
          "IH: IH_CNTL: credit 16, clean 16, VMID 0, RPTR_REARM with MSI");
    check(then(&p, &at, AMDGPU_CP_INT_CNTL_RING0, (1u << 19) | (1u << 20)),
          "IH: CP ring 0 keeps only context busy and empty");
    check(then(&p, &at, AMDGPU_DMA_CNTL, AMDGPU_CTXEMPTY_INT), "IH: DMA 0's trap off, the rest kept");
    check(then(&p, &at, AMDGPU_DMA_CNTL + 0x800u, 0), "IH: DMA 1's trap off");
    check(then(&p, &at, AMDGPU_INT_MASK + amdgpu_crtc_offset(5), 0), "IH: the sixth controller's interrupts masked");
    check(then(&p, &at, AMDGPU_GRPH_INT_CONTROL + amdgpu_crtc_offset(5), 0), "IH: and its flip interrupt");
    check(then(&p, &at, AMDGPU_IH_CNTL, (0x10u << 15) | (0x10u << 20) | (1u << 4) | 1u), "IH: interrupts on");
    check(then(&p, &at, AMDGPU_IH_RB_CNTL, rb | 1u), "IH: and last, the ring on");
    check(at == (int)p.n, "IH: and nothing after it");

    /* Refusals. */
    c.ring_bytes = 0x18000;
    check(!amdgpu_ih_setup(&p, &c), "IH: a ring not a power of two is refused");
    c.ring_bytes = 0x10000;
    c.ring_mc += 0x80u;
    check(!amdgpu_ih_setup(&p, &c), "IH: a ring not 256-byte aligned is refused");

    /* An entry: DMA 0's trap, ring 0, VMID 3. */
    dw[0] = 0xffffff00u | 224u;
    dw[1] = 0xf0000000u | 0x1234567u;
    dw[2] = 0xffff0000u | (3u << 8) | 0x00u;
    dw[3] = 0xdeadbeefu;
    amdgpu_ih_decode(dw, &iv);
    check(iv.src_id == 224u, "IV: source in dw0 7:0");
    check(iv.src_data == 0x1234567u, "IV: data in dw1 27:0");
    check(iv.ring_id == 0u && iv.vmid == 3u, "IV: ring 7:0 and VMID 15:8 of dw2");
    check(strcmp(amdgpu_ih_source_name(224), "DMA 0's trap") == 0, "IV: 224 is DMA 0's trap");
    check(amdgpu_ih_source_name(181) != NULL && amdgpu_ih_source_name(7) == NULL, "IV: 181 named, 7 not");

    check(amdgpu_ih_wptr(0x10010u | 1u, 0x10000, &ovf) == 0x10u && ovf, "IV: wptr masked to the ring, overflow seen");
    check(amdgpu_ih_wptr(0x40u, 0x10000, &ovf) == 0x40u && !ovf, "IV: wptr without overflow");
    check(amdgpu_ih_next(0xfff0u, 0x10000) == 0u, "IV: the read pointer wraps");
}

/*----------------------------------------------------------------------
 * V2: the GART and programs' address spaces.
 *--------------------------------------------------------------------*/

static uint64_t mem[0x40000 / 8];   /* 256 KB of "VRAM" at MC 0x100000 */

static uint64_t read_mem(void *env, uint64_t mc)
{
    (void)env;
    if (mc < 0x100000u || mc >= 0x100000u + sizeof mem) {
        return 0;
    }
    return mem[(mc - 0x100000u) / 8u];
}

static void check_vm(void)
{
    struct amdgpu_vm_geometry g;
    struct amdgpu_gart_cfg gc;
    struct amdgpu_vm_fault f;
    struct amdgpu_prog p;
    uint64_t size, start, end, e;
    uint32_t di, ti;
    const char *why;
    int at;
    unsigned i;

    /* radeon_device.c 1179-1190, worked by hand. */
    check(amdgpu_vm_geometry(8, &g) && g.block_size == 12u && g.pd_entries == 512u,
          "VM: 8 GB: block 12, a 4 KB page directory");
    check(amdgpu_vm_geometry(64, &g) && g.block_size == 13u && g.pd_entries == 2048u && g.pt_entries == 8192u,
          "VM: 64 GB: block 13 (27 / 2), 2048 PDEs, 8192 PTEs");
    check(g.max_pfn == 64u << 18, "VM: 64 GB is 2^24 pages");
    check(amdgpu_vm_geometry(1024, &g) && g.block_size == 15u && g.pd_entries == 8192u,
          "VM: 1 TB: block 15");
    check(!amdgpu_vm_geometry(3, &g) && !amdgpu_vm_geometry(0, &g) && !amdgpu_vm_geometry(2048, &g),
          "VM: 3, 0 and 2048 GB refused");

    amdgpu_vm_geometry(64, &g);
    check(amdgpu_vm_index(&g, 0x123456780ull, &di, &ti), "VM: an address inside is indexed");
    check(amdgpu_vm_index(&g, (5ull << 25) | (7ull << 12), &di, &ti) && di == 5u && ti == 7u,
          "VM: PDE from bits 37:25, PTE from 24:12, at block 13");
    check(!amdgpu_vm_index(&g, 64ull << 30, &di, &ti), "VM: the first address past 64 GB is outside");

    /* Entries. */
    check(amdgpu_pte(0x0123456789abcull, AMDGPU_PTE_SYSTEM_RW) == (0x0123456789000ull & 0xfffffff000ull) + 0x67u,
          "VM: a system page: address 39:12, VALID SYSTEM SNOOPED READABLE WRITEABLE = 0x67");
    check(amdgpu_pde(0x100000u) == 0x100001u, "VM: a PDE is the table and VALID");
    check(AMDGPU_PTE_FRAG(4) == 0x200u, "VM: fragment in 11:7");

    /* A walk: PD at 0x100000, one table at 0x104000, VA 3:9 mapped. */
    memset(mem, 0, sizeof mem);
    mem[3] = amdgpu_pde(0x104000u);
    mem[(0x104000u - 0x100000u) / 8u + 9u] = amdgpu_pte(0xabcd000u, AMDGPU_PTE_SYSTEM_RW);
    check(amdgpu_vm_walk(&g, read_mem, NULL, 0x100000u, (3ull << 25) | (9ull << 12) | 0x123u, &e, &why)
          && amdgpu_pte_address(e) == 0xabcd000u, "VM: the walk finds the page");
    check(!amdgpu_vm_walk(&g, read_mem, NULL, 0x100000u, (3ull << 25) | (10ull << 12), &e, &why)
          && strcmp(why, "no page") == 0, "VM: the next page is not there");
    check(!amdgpu_vm_walk(&g, read_mem, NULL, 0x100000u, (4ull << 25), &e, &why)
          && strcmp(why, "no page table") == 0, "VM: nor the next table");

    check(amdgpu_vm_base_reg(0) == 0x153cu && amdgpu_vm_base_reg(7) == 0x1558u
          && amdgpu_vm_base_reg(8) == 0x1438u && amdgpu_vm_base_reg(15) == 0x1454u,
          "VM: VMIDs 0-7 at 0x153c, 8-15 at 0x1438 (si.c 4317-4323)");

    /* radeon_gtt_location: VRAM at the bottom, the GTT goes after it... */
    size = 1ull << 30;
    amdgpu_gtt_place(0, (3ull << 30) - 1u, (1ull << 40) - 1u, &size, &start, &end);
    check(start == 3ull << 30 && end == (4ull << 30) - 1u && size == 1ull << 30, "GTT: after VRAM at nought");
    /* ...and before it, when VRAM is at the top. */
    size = 1ull << 30;
    amdgpu_gtt_place((1ull << 40) - (3ull << 30), (1ull << 40) - 1u, (1ull << 40) - 1u, &size, &start, &end);
    check(end == (1ull << 40) - (3ull << 30) - 1u && size == 1ull << 30, "GTT: before VRAM at the top");

    /* The GART enable. */
    gc.table_mc = 0xf400200000ull;
    gc.gtt_start = 0x0ull;
    gc.gtt_end = (1ull << 30) - 1u;
    gc.dummy_page = 0x7f000000ull;
    gc.vm = &g;
    check(amdgpu_gart_enable(&p, &gc), "GART: the program is made");
    check(p.w[0].reg == AMDGPU_MC_VM_MX_L1_TLB_CNTL && p.w[0].value == 0x55bu,
          "GART: L1 TLB first: 0xA << 7 | ENABLE | FRAGMENT | NOT_IN_SYS | ADVANCED_DRIVER_MODEL = 0x55b");
    at = 1;
    check(then(&p, &at, AMDGPU_VM_L2_CNTL, 0x1u | 0x2u | 0x200u | 0x400u | (7u << 15) | (1u << 19)),
          "GART: VM_L2_CNTL as si.c 4286-4291");
    check(then(&p, &at, AMDGPU_VM_L2_CNTL2, 3u), "GART: both caches invalidated");
    check(then(&p, &at, AMDGPU_VM_L2_CNTL3, (1u << 20) | 4u | (4u << 15)), "GART: VM_L2_CNTL3: BIGK 4, bank 4");
    check(then(&p, &at, AMDGPU_VM_CONTEXT0_PT_START, 0) && then(&p, &at, AMDGPU_VM_CONTEXT0_PT_END, 0x3ffffu),
          "GART: context 0 spans the GTT, in pages");
    check(then(&p, &at, AMDGPU_VM_CONTEXT0_PT_BASE, (uint32_t)(gc.table_mc >> 12)), "GART: its table, >> 12");
    check(then(&p, &at, AMDGPU_VM_CONTEXT0_FAULT_DEFAULT, 0x7f000u), "GART: faults to the dummy page");
    check(then(&p, &at, AMDGPU_VM_CONTEXT0_CNTL, 0x11u), "GART: context 0 on, flat, range faults to default");
    check(then(&p, &at, 0x15d4u, 0) && then(&p, &at, 0x15d8u, 0) && then(&p, &at, 0x15dcu, 0),
          "GART: the three unnamed registers cleared");
    check(then(&p, &at, AMDGPU_VM_CONTEXT1_PT_END, g.max_pfn - 1u), "GART: contexts 1-15 span 64 GB");
    for (i = 1; i < 16; i++) {
        check(find(&p, 0, amdgpu_vm_base_reg(i), (uint32_t)(gc.table_mc >> 12), 0) >= 0,
              "GART: every VMID given a valid base");
    }
    check(find(&p, 0, amdgpu_vm_base_reg(0), 0, 1) == find(&p, 0, AMDGPU_VM_CONTEXT0_PT_BASE, 0, 1),
          "GART: VMID 0's base written once, as context 0's");
    check(then(&p, &at, AMDGPU_VM_CONTEXT1_CNTL2, 4), "GART: CONTEXT1_CNTL2 = 4");
    check(then(&p, &at, AMDGPU_VM_CONTEXT1_CNTL, 0x040db6dbu),
          "GART: CONTEXT1_CNTL = ENABLE | DEPTH 1 | BLOCK 13-9 | every fault interrupting and to default");
    check(then(&p, &at, AMDGPU_HDP_MEM_FLUSH, 1) && then(&p, &at, AMDGPU_VM_INVALIDATE_REQUEST, 1),
          "GART: and last, the HDP flushed and context 0's TLB");
    check(at == (int)p.n, "GART: nothing after the flush");

    gc.table_mc += 0x800u;
    check(!amdgpu_gart_enable(&p, &gc), "GART: a table not page-aligned refused");

    /* The system aperture written only when it is not VRAM already. */
    amdgpu_prog_clear(&p);
    amdgpu_sys_aperture(&p, 0xf400000000ull, 0xf4ffffffffull, 0xf400001000ull, 0xf400000u, 0xf4fffffu);
    check(p.n == 0, "APER: left alone when the firmware set it to VRAM");
    amdgpu_sys_aperture(&p, 0xf400000000ull, 0xf4ffffffffull, 0xf400001000ull, 0, 0);
    check(p.n == 3 && p.w[0].value == 0xf400000u && p.w[1].value == 0xf4fffffu && p.w[2].value == 0xf400001u,
          "APER: otherwise low, high, default, in pages");

    /* A fault (sid.h 422-435): VMID 2 wrote, client 0x41, the valid bit. */
    amdgpu_vm_fault_decode((2u << 25) | (1u << 24) | (0x41u << 12) | 0x4u, 0x12345u, &f);
    check(f.vmid == 2u && f.write && f.client == 0x41u && f.protections == 4u && f.address == 0x12345000u,
          "FAULT: VMID 28:25, write 24, client 19:12, protections 3:0, the page << 12");
}

/*----------------------------------------------------------------------
 * The DMA engine's packets for V1, V2 and V4.
 *--------------------------------------------------------------------*/

static void check_dma(void)
{
    uint32_t w[32];
    unsigned n, i;

    check(amdgpu_dma_trap(w) == 1 && w[0] == 0x70000000u, "DMA: TRAP is 7 << 28");

    /* si_dma_ring_emit_ib: NOPs until wptr % 8 == 5. */
    n = amdgpu_dma_ib(w, 0, 3, 0xf400123440ull, 0x40);
    check(n == 8, "DMA IB at 0: five NOPs, then three");
    for (i = 0; i < 5; i++) {
        check(w[i] == 0xf0000000u, "DMA IB: the padding is NOPs");
    }
    check(w[5] == ((4u << 28) | (3u << 20)), "DMA IB: INDIRECT_BUFFER 4 << 28, VMID 23:20");
    check(w[6] == 0x00123440u, "DMA IB: the address low, 32-byte aligned");
    check(w[7] == ((0x40u << 12) | 0xf4u), "DMA IB: length << 12, high byte");
    check(amdgpu_dma_ib(w, 5, 0, 0, 1) == 3, "DMA IB at 5: no padding");
    check(amdgpu_dma_ib(w, 6, 0, 0, 1) == 10, "DMA IB at 6: seven NOPs");
    check(amdgpu_dma_ib(w, 13, 0, 0x1fu, 1) == 3 && w[1] == 0, "DMA IB: address bits 4:0 dropped");

    /* si_dma_vm_flush: three SRBM_WRITEs and a POLL_REG_MEM. */
    n = amdgpu_dma_vm_flush(w, 9, 0xf400340000ull);
    check(n == 15, "DMA VM flush: 15 dwords");
    check(w[0] == 0x90000000u && w[1] == ((0xfu << 16) | (0x143cu >> 2)) && w[2] == 0xf400340u,
          "DMA VM flush: VMID 9's base, 0x1438 + 4, by SRBM_WRITE with byte enables 0xf");
    check(w[4] == ((0xfu << 16) | (0x5480u >> 2)) && w[5] == 1u, "DMA VM flush: the HDP");
    check(w[7] == ((0xfu << 16) | (0x1478u >> 2)) && w[8] == (1u << 9), "DMA VM flush: invalidate VMID 9");
    check(w[9] == 0xe0000000u && w[10] == 0x51eu && w[11] == (0xffu << 16) && w[12] == (1u << 9)
          && w[13] == 0 && w[14] == 0x20u, "DMA VM flush: POLL_REG_MEM as amdgpu's si_dma.c 444-449");
}

/*----------------------------------------------------------------------
 * V3 and V4: the RLC, the CP, PM4.
 *--------------------------------------------------------------------*/

static void check_cp(void)
{
    struct amdgpu_prog p;
    uint32_t w[64];
    unsigned n;
    int at;

    /* Programs. */
    amdgpu_rlc_stop(&p, 0xffffffffu, 0x5u);
    check(p.w[0].reg == AMDGPU_RLC_CNTL && p.w[0].value == 0, "RLC: stopped first");
    at = 1;
    check(then(&p, &at, AMDGPU_CP_INT_CNTL_RING0, ~((1u << 19) | (1u << 20))), "RLC: GUI idle interrupt off");
    check(then(&p, &at, AMDGPU_WR_POLL, AMDGPU_DB_DEPTH_INFO) && p.w[at].reg == 0,
          "RLC: a gfx register read, as Linux does before waiting");
    check(then(&p, &at, AMDGPU_WR_POLL, AMDGPU_RLC_STAT) && p.w[at].reg == 0xfu && p.w[at].value == 0x6u,
          "RLC: RLC_STAT & 0xf waited for to read clock and power on");
    check(then(&p, &at, AMDGPU_WR_POLL, AMDGPU_RLC_SERDES_BUSY_0) && p.w[at].value == 0, "RLC: SERDES 0 idle");
    check(then(&p, &at, AMDGPU_WR_POLL, AMDGPU_RLC_SERDES_BUSY_1), "RLC: SERDES 1 idle");
    check(then(&p, &at, AMDGPU_GRBM_SOFT_RESET, 0x5u | 0x4u) && then(&p, &at, AMDGPU_WR_DELAY, 50)
          && then(&p, &at, AMDGPU_GRBM_SOFT_RESET, 0x1u) && then(&p, &at, AMDGPU_WR_DELAY, 50),
          "RLC: SOFT_RESET_RLC pulsed for 50 us, the other bits kept");
    check(amdgpu_prog_last(&p, 0xfu, 0xabcu) == 0xabcu, "RLC: a poll's mask and value are not a write");

    check(amdgpu_rlc_prepare(&p, 0xf400300000ull, 0xf400301000ull), "RLC: prepared");
    check(p.w[0].reg == AMDGPU_RLC_SAVE_RESTORE_BASE && p.w[0].value == 0xf4003000u
          && p.w[1].reg == AMDGPU_RLC_CLEAR_STATE_BASE && p.w[1].value == 0xf4003010u,
          "RLC: the two buffers, >> 8, first (gfx_v6_0_init_pg without PG)");
    check(amdgpu_prog_last(&p, AMDGPU_RLC_LB_CNTR_MAX, 0) == 0xffffffffu
          && amdgpu_prog_last(&p, AMDGPU_RLC_LB_INIT_CU_MASK, 0) == 0xffffffffu, "RLC: the load balancer's counters");
    check(!amdgpu_rlc_prepare(&p, 0xf400300080ull, 0), "RLC: a buffer not 256-aligned refused");

    amdgpu_rlc_start(&p, 0);
    at = 0;
    check(then(&p, &at, AMDGPU_GRBM_GFX_INDEX, 0xe0000000u) && then(&p, &at, AMDGPU_SPI_LB_CU_MASK, 0xffu),
          "RLC: load balancing off: broadcast, SPI_LB_CU_MASK 0xff");
    check(then(&p, &at, AMDGPU_RLC_CNTL, 1) && then(&p, &at, AMDGPU_CP_INT_CNTL_RING0, (1u << 19) | (1u << 20)),
          "RLC: started, then its GUI idle interrupt");

    amdgpu_cp_halt(&p);
    check(p.n == 3 && p.w[0].reg == AMDGPU_CP_ME_CNTL && p.w[0].value == 0x15000000u && p.w[2].value == 50u,
          "CP: halted - ME 28, PFP 26, CE 24 - and 50 us");
    amdgpu_cp_loaded(&p);
    check(p.n == 4 && p.w[3].reg == AMDGPU_CP_ME_RAM_RADDR, "CP: the four address registers nought after the load");

    check(amdgpu_cp_rb_cntl(0x10000) == ((9u << 8) | 13u), "CP: RB0_CNTL for 64 KB: block 9, size 13");
    check(amdgpu_cp_rb_cntl(0x100000) == ((9u << 8) | 17u), "CP: RB0_CNTL for Linux's 1 MB ring: 17");
    check(amdgpu_cp_resume(&p, 0xf400400000ull, 0x10000, 0xf400410000ull), "CP: resume made");
    at = 0;
    check(then(&p, &at, AMDGPU_CP_SEM_WAIT_TIMER, 0) && then(&p, &at, AMDGPU_CP_SEM_INCOMPLETE, 0)
          && then(&p, &at, AMDGPU_CP_RB_WPTR_DELAY, 0) && then(&p, &at, AMDGPU_CP_DEBUG, 0)
          && then(&p, &at, AMDGPU_SCRATCH_ADDR, 0), "CP: the timers, delay, debug and scratch nought");
    check(then(&p, &at, AMDGPU_CP_RB0_CNTL, 0x90du) && then(&p, &at, AMDGPU_CP_RB0_CNTL, 0x8000090du),
          "CP: RB0_CNTL, then with RPTR_WR_ENA");
    check(then(&p, &at, AMDGPU_CP_RB0_WPTR, 0) && then(&p, &at, AMDGPU_CP_RB0_RPTR_ADDR, 0x00410000u)
          && then(&p, &at, AMDGPU_CP_RB0_RPTR_ADDR_HI, 0xf4u), "CP: WPTR nought, the read pointer's write-back");
    check(then(&p, &at, AMDGPU_WR_DELAY, 1000) && then(&p, &at, AMDGPU_CP_RB0_CNTL, 0x90du)
          && then(&p, &at, AMDGPU_CP_RB0_BASE, 0xf4004000u), "CP: 1 ms, RPTR_WR_ENA off, then the base >> 8");
    check(!amdgpu_cp_resume(&p, 0xf400400000ull, 0x3000, 0), "CP: a ring not a power of two refused");

    /* PM4. */
    check(amdgpu_pm4(AMDGPU_PM4_SET_CONFIG_REG, 1) == 0xc0016800u, "PM4: SET_CONFIG_REG, 1 = 0xc0016800");
    n = amdgpu_pm4_ring_test(w);
    check(n == 3 && w[1] == 0x140u && w[2] == 0xdeadbeefu, "PM4: the ring test writes SCRATCH_REG0, (0x8500 - 0x8000) >> 2");
    check(amdgpu_pm4_set_config(w, 0x28000u, 0) == 0, "PM4: a context register refused by SET_CONFIG_REG");

    n = amdgpu_pm4_me_init(w, 8);
    check(n == 11 && w[0] == 0xc0054400u && w[1] == 1u && w[3] == 7u && w[4] == 0x10000u,
          "PM4: ME_INITIALIZE, 5: 1, 0, 7, DEVICE_ID(1)");
    check(w[7] == 0xc0021100u && w[8] == 3u && w[9] == 0xc000u && w[10] == 0xe000u, "PM4: SET_BASE of the CE partition");

    n = amdgpu_pm4_fence(w, 0xf400500008ull, 0x1122334455ull, true, true);
    check(n == 14, "PM4: a fence is 14 dwords");
    check(w[1] == (0x85e8u - 0x8000u) >> 2 && w[2] == 0, "PM4: CP_COHER_CNTL2 nought");
    check(w[3] == 0xc0034300u && w[4] == 0x28c00000u && w[5] == 0xffffffffu && w[7] == 10u,
          "PM4: SURFACE_SYNC of TCL1, TC, SH_KCACHE, SH_ICACHE");
    check(w[8] == 0xc0044700u && w[9] == 0x514u, "PM4: EVENT_WRITE_EOP: CACHE_FLUSH_AND_INV_TS (20), index 5");
    check(w[10] == 0x00500008u && w[11] == (0xf4u | (2u << 29) | (2u << 24)),
          "PM4: EOP address, DATA_SEL 2 (64 bits), INT_SEL 2");
    check(w[12] == 0x22334455u && w[13] == 0x11u, "PM4: the sequence, low then high");
    amdgpu_pm4_fence(w, 0, 1, false, false);
    check(w[11] == (1u << 29), "PM4: a 32-bit fence without an interrupt: DATA_SEL 1, INT_SEL 0");

    n = amdgpu_pm4_ib(w, 0xf400600004ull, 0x30, 5, false, true);
    check(n == 6 && w[0] == 0xc0008b00u && w[2] == 0xc0023200u && w[3] == 0x00600004u
          && w[4] == 0xf4u && w[5] == (0x30u | (5u << 24)),
          "PM4: SWITCH_BUFFER, then INDIRECT_BUFFER: address, high 16 bits, length | VMID << 24");
    amdgpu_pm4_ib(w, 0, 1, 0, true, false);
    check(w[0] == 0xc0023100u, "PM4: the CE's INDIRECT_BUFFER_CONST");

    n = amdgpu_pm4_vm_flush(w, 3, 0xf400700000ull);
    check(n == 24, "PM4: a VM flush is 24 dwords");
    check(w[0] == 0xc0033700u && w[1] == 0x40000000u && w[2] == (0x1548u >> 2) && w[4] == 0xf400700u,
          "PM4: VMID 3's base by WRITE_DATA, the PFP, a register");
    check(w[7] == (0x5480u >> 2) && w[12] == (0x1478u >> 2) && w[14] == 8u, "PM4: HDP flush, invalidate VMID 3");
    check(w[15] == 0xc0053c00u && w[16] == 0 && w[17] == (0x1478u >> 2) && w[21] == 0x20u,
          "PM4: WAIT_REG_MEM always on VM_INVALIDATE_REQUEST, poll 0x20");
    check(w[22] == 0xc0004200u && w[23] == 0, "PM4: PFP_SYNC_ME");

    check(amdgpu_pm4_pad(w, 1) == 1 && w[0] == 0x80000000u, "PM4: one dword of padding is a PACKET2");
    check(amdgpu_pm4_pad(w, 4) == 4 && w[0] == 0xc0021000u, "PM4: four is a NOP saying three follow");
}

/*----------------------------------------------------------------------
 * The firmware the CP and RLC take.
 *--------------------------------------------------------------------*/

static void check_fw(const char *dir)
{
    static const char *const chips[] = { "tahiti", "pitcairn" };
    static const char *const names[] = { "pfp", "me", "ce", "rlc" };
    static const enum amdgpu_fw_kind kinds[] = { AMDGPU_FW_PFP, AMDGPU_FW_ME, AMDGPU_FW_CE, AMDGPU_FW_RLC };
    struct amdgpu_fw fw;
    char file[64], what[128];
    unsigned c, k;

    for (c = 0; c < 2; c++) {
        for (k = 0; k < 4; k++) {
            size_t len;
            uint8_t *b;
            const char *err;

            snprintf(file, sizeof file, "%s_%s.bin", chips[c], names[k]);
            b = (uint8_t *)slurp(dir, file, &len);
            snprintf(what, sizeof what, "FW: %s read", file);
            check(b != NULL, what);
            if (b == NULL) {
                continue;
            }

            err = amdgpu_fw_parse(kinds[k], b, len, &fw);
            snprintf(what, sizeof what, "FW: %s parses (%s)", file, err ? err : "ok");
            check(err == NULL, what);
            if (err == NULL) {
                snprintf(what, sizeof what, "FW: %s's payload is the size the engine takes", file);
                check(fw.count == (k == 3 ? 2048u : 2144u) && fw.words == b + 256, what);
            }

            /* Controls on the real bytes: each refusal is reached. */
            snprintf(what, sizeof what, "FW: %s one byte short refused", file);
            check(amdgpu_fw_parse(kinds[k], b, len - 1u, &fw) != NULL, what);
            b[12] = 7;
            snprintf(what, sizeof what, "FW: %s as IP 7.0 refused", file);
            check(amdgpu_fw_parse(kinds[k], b, len, &fw) != NULL, what);
            b[12] = 6;
            snprintf(what, sizeof what, "FW: %s taken as the other kind refused", file);
            check(amdgpu_fw_parse(k == 3 ? AMDGPU_FW_PFP : AMDGPU_FW_RLC, b, len, &fw) != NULL, what);
            free(b);
        }
    }
}

int main(int argc, char **argv)
{
    check_ih();
    check_vm();
    check_dma();
    check_cp();

    if (argc > 1) {
        check_offsets(argv[1]);
    } else {
        skipped++;
        printf("note: no header directory given; the offsets against amdgpu's *_d.h not checked\n");
    }
    if (argc > 2) {
        check_fw(argv[2]);
    } else {
        skipped++;
        printf("note: no firmware directory given; the firmware not parsed\n");
    }

    printf("amdgpu engine: %d of %d checks pass%s\n", checks - failed, checks,
           skipped ? " (some skipped)" : "");
    return failed == 0 ? 0 : 1;
}
