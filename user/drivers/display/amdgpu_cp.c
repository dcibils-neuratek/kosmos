/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A Southern Islands GPU's command processor: firmware checked, bring-up
 * as programs, PM4 packets built (`amdgpu_cp.h`, `docs/amdgpu.md` V3-V4).
 * Pure: no hardware.
 */

#include "amdgpu_cp.h"
#include "amdgpu_decode.h"
#include "amdgpu_vm.h"

/*----------------------------------------------------------------------
 * Firmware.
 *--------------------------------------------------------------------*/

static uint32_t le32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint16_t le16(const uint8_t *b)
{
    return (uint16_t)(b[0] | (b[1] << 8));
}

const char *amdgpu_fw_parse(enum amdgpu_fw_kind kind, const uint8_t *bytes, size_t len,
                            struct amdgpu_fw *out)
{
    uint32_t size, hdr, ucode_bytes, offset, want, hdr_min;

    switch (kind) {
    case AMDGPU_FW_PFP: want = AMDGPU_PFP_WORDS; hdr_min = 44u; break;
    case AMDGPU_FW_ME:  want = AMDGPU_ME_WORDS;  hdr_min = 44u; break;
    case AMDGPU_FW_CE:  want = AMDGPU_CE_WORDS;  hdr_min = 44u; break;
    case AMDGPU_FW_RLC: want = AMDGPU_RLC_WORDS; hdr_min = 52u; break;
    default: return "not a kind of firmware this driver loads";
    }

    if (bytes == NULL || len < hdr_min) {
        return "shorter than its header";
    }

    size = le32(bytes + 0);
    hdr = le32(bytes + 4);
    ucode_bytes = le32(bytes + 20);
    offset = le32(bytes + 24);

    if (size != len) {
        return "not the size its header says";
    }
    if (hdr < hdr_min || hdr > len) {
        return "a header of the wrong size";
    }
    if (le16(bytes + 8) != 1u || le16(bytes + 10) != 0u) {
        return "a header version other than 1.0";
    }
    if (le16(bytes + 12) != 6u || le16(bytes + 14) != 0u) {
        return "for another generation than Southern Islands";
    }
    if (offset < hdr || (offset & 3u) != 0 || offset > len || ucode_bytes > len - offset) {
        return "its payload outside the file";
    }
    if (ucode_bytes != want * 4u) {
        return "a payload of the wrong length";
    }

    out->words = bytes + offset;
    out->count = want;
    out->ucode_version = le32(bytes + 16);
    out->feature_version = le32(bytes + 32);
    return NULL;
}

uint32_t amdgpu_fw_word(const struct amdgpu_fw *fw, uint32_t i)
{
    return le32(fw->words + (size_t)i * 4u);
}

/*----------------------------------------------------------------------
 * Bring-up.
 *--------------------------------------------------------------------*/

void amdgpu_rlc_stop(struct amdgpu_prog *p, uint32_t cp_int_cntl, uint32_t soft_reset)
{
    amdgpu_prog_clear(p);

    /* `gfx_v6_0_rlc_stop`. */
    amdgpu_prog_wr(p, AMDGPU_RLC_CNTL, 0);
    amdgpu_prog_wr(p, AMDGPU_CP_INT_CNTL_RING0,
                   cp_int_cntl & ~(AMDGPU_CNTX_BUSY_INT_ENABLE | AMDGPU_CNTX_EMPTY_INT_ENABLE));
    amdgpu_prog_poll(p, AMDGPU_DB_DEPTH_INFO, 0, 0);
    amdgpu_prog_poll(p, AMDGPU_RLC_STAT,
                     AMDGPU_RLC_BUSY_STATUS | AMDGPU_GFX_POWER_STATUS
                     | AMDGPU_GFX_CLOCK_STATUS | AMDGPU_GFX_LS_STATUS,
                     AMDGPU_GFX_CLOCK_STATUS | AMDGPU_GFX_POWER_STATUS);
    amdgpu_prog_poll(p, AMDGPU_RLC_SERDES_BUSY_0, 0xffffffffu, 0);
    amdgpu_prog_poll(p, AMDGPU_RLC_SERDES_BUSY_1, 0xffffffffu, 0);

    /* `gfx_v6_0_rlc_reset`. */
    amdgpu_prog_wr(p, AMDGPU_GRBM_SOFT_RESET, soft_reset | AMDGPU_SOFT_RESET_RLC);
    amdgpu_prog_delay(p, 50);
    amdgpu_prog_wr(p, AMDGPU_GRBM_SOFT_RESET, soft_reset & ~AMDGPU_SOFT_RESET_RLC);
    amdgpu_prog_delay(p, 50);
}

bool amdgpu_rlc_prepare(struct amdgpu_prog *p, uint64_t save_restore_mc, uint64_t clear_state_mc)
{
    amdgpu_prog_clear(p);

    if ((save_restore_mc & 0xffu) != 0 || (clear_state_mc & 0xffu) != 0) {
        return false;
    }

    amdgpu_prog_wr(p, AMDGPU_RLC_SAVE_RESTORE_BASE, (uint32_t)(save_restore_mc >> 8));
    amdgpu_prog_wr(p, AMDGPU_RLC_CLEAR_STATE_BASE, (uint32_t)(clear_state_mc >> 8));

    amdgpu_prog_wr(p, AMDGPU_RLC_RL_BASE, 0);
    amdgpu_prog_wr(p, AMDGPU_RLC_RL_SIZE, 0);
    amdgpu_prog_wr(p, AMDGPU_RLC_LB_CNTL, 0);
    amdgpu_prog_wr(p, AMDGPU_RLC_LB_CNTR_MAX, 0xffffffffu);
    amdgpu_prog_wr(p, AMDGPU_RLC_LB_CNTR_INIT, 0);
    amdgpu_prog_wr(p, AMDGPU_RLC_LB_INIT_CU_MASK, 0xffffffffu);
    amdgpu_prog_wr(p, AMDGPU_RLC_MC_CNTL, 0);
    amdgpu_prog_wr(p, AMDGPU_RLC_UCODE_CNTL, 0);
    return !p->overflow;
}

void amdgpu_rlc_start(struct amdgpu_prog *p, uint32_t cp_int_cntl)
{
    amdgpu_prog_clear(p);

    /* `gfx_v6_0_enable_lbpw(false)` (2388-2396): LOAD_BALANCE_ENABLE, bit 0
     * of a register the prepare left nought, and every SE and SH selected
     * for SPI_LB_CU_MASK. */
    amdgpu_prog_wr(p, AMDGPU_RLC_LB_CNTL, 0);
    amdgpu_prog_wr(p, AMDGPU_GRBM_GFX_INDEX, AMDGPU_GFX_BROADCAST);
    amdgpu_prog_wr(p, AMDGPU_SPI_LB_CU_MASK, 0x00ffu);

    /* `gfx_v6_0_rlc_start` (2448-2455). */
    amdgpu_prog_wr(p, AMDGPU_RLC_CNTL, AMDGPU_RLC_ENABLE);
    amdgpu_prog_wr(p, AMDGPU_CP_INT_CNTL_RING0,
                   cp_int_cntl | AMDGPU_CNTX_BUSY_INT_ENABLE | AMDGPU_CNTX_EMPTY_INT_ENABLE);
    amdgpu_prog_delay(p, 50);
}

void amdgpu_cp_halt(struct amdgpu_prog *p)
{
    amdgpu_prog_clear(p);
    amdgpu_prog_wr(p, AMDGPU_CP_ME_CNTL, AMDGPU_CP_ME_HALT | AMDGPU_CP_PFP_HALT | AMDGPU_CP_CE_HALT);
    amdgpu_prog_wr(p, AMDGPU_SCRATCH_UMSK, 0);
    amdgpu_prog_delay(p, 50);
}

void amdgpu_cp_loaded(struct amdgpu_prog *p)
{
    amdgpu_prog_clear(p);
    amdgpu_prog_wr(p, AMDGPU_CP_PFP_UCODE_ADDR, 0);
    amdgpu_prog_wr(p, AMDGPU_CP_CE_UCODE_ADDR, 0);
    amdgpu_prog_wr(p, AMDGPU_CP_ME_RAM_WADDR, 0);
    amdgpu_prog_wr(p, AMDGPU_CP_ME_RAM_RADDR, 0);
}

static uint32_t order_base_2(uint32_t x)
{
    uint32_t n = 0;

    while ((1u << n) < x && n < 31u) {
        n++;
    }
    return n;
}

uint32_t amdgpu_cp_rb_cntl(uint32_t ring_bytes)
{
    return (order_base_2(4096u / 8u) << 8) | order_base_2(ring_bytes / 8u);
}

bool amdgpu_cp_resume(struct amdgpu_prog *p, uint64_t ring_mc, uint32_t ring_bytes,
                      uint64_t rptr_mc)
{
    uint32_t cntl;

    amdgpu_prog_clear(p);

    if (ring_bytes < 0x1000u || ring_bytes > 0x100000u || (ring_bytes & (ring_bytes - 1u)) != 0
        || (ring_mc & 0xffu) != 0 || (rptr_mc & 3u) != 0) {
        return false;
    }

    cntl = amdgpu_cp_rb_cntl(ring_bytes);

    amdgpu_prog_wr(p, AMDGPU_CP_SEM_WAIT_TIMER, 0);
    amdgpu_prog_wr(p, AMDGPU_CP_SEM_INCOMPLETE, 0);
    amdgpu_prog_wr(p, AMDGPU_CP_RB_WPTR_DELAY, 0);
    amdgpu_prog_wr(p, AMDGPU_CP_DEBUG, 0);
    amdgpu_prog_wr(p, AMDGPU_SCRATCH_ADDR, 0);

    amdgpu_prog_wr(p, AMDGPU_CP_RB0_CNTL, cntl);
    amdgpu_prog_wr(p, AMDGPU_CP_RB0_CNTL, cntl | AMDGPU_RB_RPTR_WR_ENA);
    amdgpu_prog_wr(p, AMDGPU_CP_RB0_WPTR, 0);
    amdgpu_prog_wr(p, AMDGPU_CP_RB0_RPTR_ADDR, (uint32_t)rptr_mc);
    amdgpu_prog_wr(p, AMDGPU_CP_RB0_RPTR_ADDR_HI, (uint32_t)(rptr_mc >> 32) & 0xffu);
    amdgpu_prog_wr(p, AMDGPU_SCRATCH_UMSK, 0);
    amdgpu_prog_delay(p, 1000);
    amdgpu_prog_wr(p, AMDGPU_CP_RB0_CNTL, cntl);
    amdgpu_prog_wr(p, AMDGPU_CP_RB0_BASE, (uint32_t)(ring_mc >> 8));
    return !p->overflow;
}

void amdgpu_cp_run(struct amdgpu_prog *p)
{
    amdgpu_prog_clear(p);
    amdgpu_prog_wr(p, AMDGPU_CP_ME_CNTL, 0);
    amdgpu_prog_delay(p, 50);
}

/*----------------------------------------------------------------------
 * PM4.
 *--------------------------------------------------------------------*/

uint32_t amdgpu_pm4(uint32_t op, uint32_t n)
{
    return (3u << 30) | ((op & 0xffu) << 8) | ((n & 0x3fffu) << 16);
}

unsigned amdgpu_pm4_set_config(uint32_t *out, uint32_t reg, uint32_t value)
{
    if (reg < AMDGPU_CONFIG_REG_START || reg >= AMDGPU_CONFIG_REG_END || (reg & 3u) != 0) {
        return 0;
    }

    out[0] = amdgpu_pm4(AMDGPU_PM4_SET_CONFIG_REG, 1);
    out[1] = (reg - AMDGPU_CONFIG_REG_START) >> 2;
    out[2] = value;
    return 3;
}

#define DST_SEL(x)     ((uint32_t)(x) << 8)
#define WR_CONFIRM     (1u << 20)
#define ENGINE_SEL(x)  ((uint32_t)(x) << 30)

unsigned amdgpu_pm4_write_mem(uint32_t *out, uint64_t addr, uint32_t value)
{
    out[0] = amdgpu_pm4(AMDGPU_PM4_WRITE_DATA, 3);
    out[1] = ENGINE_SEL(0) | DST_SEL(5) | WR_CONFIRM;
    out[2] = (uint32_t)addr & 0xfffffffcu;
    out[3] = (uint32_t)(addr >> 32);
    out[4] = value;
    return 5;
}

unsigned amdgpu_pm4_write_reg(uint32_t *out, uint32_t reg, uint32_t value)
{
    out[0] = amdgpu_pm4(AMDGPU_PM4_WRITE_DATA, 3);
    out[1] = ENGINE_SEL(1) | DST_SEL(0);
    out[2] = reg >> 2;
    out[3] = 0;
    out[4] = value;
    return 5;
}

unsigned amdgpu_pm4_me_init(uint32_t *out, uint32_t max_hw_contexts)
{
    out[0] = amdgpu_pm4(AMDGPU_PM4_ME_INITIALIZE, 5);
    out[1] = 0x1u;
    out[2] = 0x0u;
    out[3] = max_hw_contexts - 1u;
    out[4] = 1u << 16;                  /* PACKET3_ME_INITIALIZE_DEVICE_ID(1) */
    out[5] = 0;
    out[6] = 0;
    out[7] = amdgpu_pm4(AMDGPU_PM4_SET_BASE, 2);
    out[8] = 3u;                        /* BASE_INDEX(CE_PARTITION_BASE) */
    out[9] = 0xc000u;
    out[10] = 0xe000u;
    return 11;
}

unsigned amdgpu_pm4_ring_test(uint32_t *out)
{
    return amdgpu_pm4_set_config(out, AMDGPU_SCRATCH_REG0, 0xdeadbeefu);
}

unsigned amdgpu_pm4_fence(uint32_t *out, uint64_t addr, uint64_t seq, bool wide, bool interrupt)
{
    unsigned n = amdgpu_pm4_set_config(out, AMDGPU_CP_COHER_CNTL2, 0);

    out[n++] = amdgpu_pm4(AMDGPU_PM4_SURFACE_SYNC, 3);
    out[n++] = (1u << 22) | (1u << 23) | (1u << 27) | (1u << 29);   /* TCL1, TC, SH_KCACHE, SH_ICACHE */
    out[n++] = 0xffffffffu;
    out[n++] = 0;
    out[n++] = 10;                                                  /* poll interval */

    out[n++] = amdgpu_pm4(AMDGPU_PM4_EVENT_WRITE_EOP, 4);
    out[n++] = 20u | (5u << 8);         /* CACHE_FLUSH_AND_INV_TS_EVENT, EVENT_INDEX(5) */
    out[n++] = (uint32_t)addr & 0xfffffffcu;
    out[n++] = ((uint32_t)(addr >> 32) & 0xffffu) | ((wide ? 2u : 1u) << 29) | ((interrupt ? 2u : 0u) << 24);
    out[n++] = (uint32_t)seq;
    out[n++] = (uint32_t)(seq >> 32);
    return n;
}

unsigned amdgpu_pm4_ib(uint32_t *out, uint64_t addr, uint32_t dwords, unsigned vmid,
                       bool ce, bool switch_buffer)
{
    unsigned n = 0;

    if (switch_buffer) {
        out[n++] = amdgpu_pm4(AMDGPU_PM4_SWITCH_BUFFER, 0);
        out[n++] = 0;
    }

    out[n++] = amdgpu_pm4(ce ? AMDGPU_PM4_IB_CONST : AMDGPU_PM4_IB, 2);
    out[n++] = (uint32_t)addr & 0xfffffffcu;
    out[n++] = (uint32_t)(addr >> 32) & 0xffffu;
    out[n++] = (dwords & 0xfffffu) | ((uint32_t)(vmid & 0xfu) << 24);
    return n;
}

unsigned amdgpu_pm4_vm_flush(uint32_t *out, unsigned vmid, uint64_t pd_mc)
{
    unsigned n = 0;

    n += amdgpu_pm4_write_reg(out + n, amdgpu_vm_base_reg(vmid), (uint32_t)(pd_mc >> 12));
    n += amdgpu_pm4_write_reg(out + n, AMDGPU_HDP_MEM_FLUSH, 1);
    n += amdgpu_pm4_write_reg(out + n, AMDGPU_VM_INVALIDATE_REQUEST, 1u << (vmid & 0xfu));

    out[n++] = amdgpu_pm4(AMDGPU_PM4_WAIT_REG_MEM, 5);
    out[n++] = 0;                       /* FUNCTION(0) always, ENGINE(0) the ME */
    out[n++] = AMDGPU_VM_INVALIDATE_REQUEST >> 2;
    out[n++] = 0;
    out[n++] = 0;                       /* reference */
    out[n++] = 0;                       /* mask */
    out[n++] = 0x20u;                   /* poll interval */

    out[n++] = amdgpu_pm4(AMDGPU_PM4_PFP_SYNC_ME, 0);
    out[n++] = 0;
    return n;
}

unsigned amdgpu_pm4_pad(uint32_t *out, unsigned count)
{
    unsigned i;

    if (count == 0) {
        return 0;
    }
    if (count == 1) {
        out[0] = 0x80000000u;           /* PACKET2: one dword, `sid.h` 1589 */
        return 1;
    }

    out[0] = amdgpu_pm4(AMDGPU_PM4_NOP, count - 2u);
    for (i = 1; i < count; i++) {
        out[i] = 0;
    }
    return count;
}
