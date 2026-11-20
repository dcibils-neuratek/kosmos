/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A Southern Islands GPU's interrupt ring, as programs and decoding
 * (`amdgpu_ih.h`, `docs/amdgpu.md` V1). Pure: no hardware, no system calls.
 */

#include "amdgpu_ih.h"
#include "amdgpu_decode.h"

void amdgpu_prog_clear(struct amdgpu_prog *p)
{
    p->n = 0;
    p->overflow = false;
}

void amdgpu_prog_wr(struct amdgpu_prog *p, uint32_t reg, uint32_t value)
{
    if (p->n >= AMDGPU_PROG_MAX) {
        p->overflow = true;
        return;
    }

    p->w[p->n].reg = reg;
    p->w[p->n].value = value;
    p->n++;
}

void amdgpu_prog_delay(struct amdgpu_prog *p, uint32_t us)
{
    amdgpu_prog_wr(p, AMDGPU_WR_DELAY, us);
}

void amdgpu_prog_poll(struct amdgpu_prog *p, uint32_t reg, uint32_t mask, uint32_t value)
{
    if (p->n + 2u > AMDGPU_PROG_MAX) {
        p->overflow = true;
        return;
    }

    amdgpu_prog_wr(p, AMDGPU_WR_POLL, reg);
    amdgpu_prog_wr(p, mask, value);
}

uint32_t amdgpu_prog_last(const struct amdgpu_prog *p, uint32_t reg, uint32_t otherwise)
{
    unsigned i;
    uint32_t v = otherwise;

    for (i = 0; i < p->n; i++) {
        if (p->w[i].reg == AMDGPU_WR_POLL) {
            i++;                        /* the mask and value: not a write */
            continue;
        }
        if (p->w[i].reg == reg) {
            v = p->w[i].value;
        }
    }

    return v;
}

/* order_base_2(bytes / 4) << 1: the same field the DMA ring has. */
uint32_t amdgpu_ih_rb_size(uint32_t ring_bytes)
{
    return amdgpu_dma_rb_size(ring_bytes);
}

bool amdgpu_ih_setup(struct amdgpu_prog *p, const struct amdgpu_ih_cfg *cfg)
{
    uint32_t rb_cntl, ih_cntl, interrupt_cntl;
    unsigned k;

    amdgpu_prog_clear(p);

    if (cfg->ring_bytes < 0x1000u || cfg->ring_bytes > 0x40000u
        || (cfg->ring_bytes & (cfg->ring_bytes - 1u)) != 0
        || (cfg->ring_mc & 0xffu) != 0 || (cfg->wptr_mc & 3u) != 0
        || (cfg->dummy_mc & 0xfffu) != 0 || cfg->crtcs > AMDGPU_CRTCS_MAX) {
        return false;
    }

    /* `si_ih_disable_interrupts` (si_ih.c 47-60). */
    amdgpu_prog_wr(p, AMDGPU_IH_RB_CNTL, cfg->ih_rb_cntl & ~AMDGPU_IH_RB_ENABLE);
    amdgpu_prog_wr(p, AMDGPU_IH_CNTL, cfg->ih_cntl & ~AMDGPU_IH_ENABLE_INTR);
    amdgpu_prog_wr(p, AMDGPU_IH_RB_RPTR, 0);
    amdgpu_prog_wr(p, AMDGPU_IH_RB_WPTR, 0);

    /* The dummy read (69-74). Linux's ring is in system memory, so it clears
     * NONSNOOP; radeon's comment (si.c 5985) says to set it for a ring in
     * VRAM, which is where this driver's is until it has a GART. */
    amdgpu_prog_wr(p, AMDGPU_INTERRUPT_CNTL2, (uint32_t)(cfg->dummy_mc >> 8));
    interrupt_cntl = cfg->interrupt_cntl & ~(AMDGPU_IH_DUMMY_RD_OVERRIDE | AMDGPU_IH_REQ_NONSNOOP_EN);
    if (cfg->in_vram) {
        interrupt_cntl |= AMDGPU_IH_REQ_NONSNOOP_EN;
    }
    amdgpu_prog_wr(p, AMDGPU_INTERRUPT_CNTL, interrupt_cntl);

    /* The ring (76-88). */
    amdgpu_prog_wr(p, AMDGPU_IH_RB_BASE, (uint32_t)(cfg->ring_mc >> 8));
    rb_cntl = AMDGPU_IH_WPTR_OVERFLOW_ENABLE | AMDGPU_IH_WPTR_OVERFLOW_CLEAR
            | amdgpu_ih_rb_size(cfg->ring_bytes);
    if (cfg->writeback) {
        rb_cntl |= AMDGPU_IH_WPTR_WRITEBACK;
    }
    amdgpu_prog_wr(p, AMDGPU_IH_RB_WPTR_ADDR_LO, (uint32_t)cfg->wptr_mc & 0xfffffffcu);
    amdgpu_prog_wr(p, AMDGPU_IH_RB_WPTR_ADDR_HI, (uint32_t)(cfg->wptr_mc >> 32) & 0xffu);
    amdgpu_prog_wr(p, AMDGPU_IH_RB_CNTL, rb_cntl);
    amdgpu_prog_wr(p, AMDGPU_IH_RB_RPTR, 0);
    amdgpu_prog_wr(p, AMDGPU_IH_RB_WPTR, 0);

    /* IH_CNTL, disabled for now (90-93). */
    ih_cntl = AMDGPU_IH_MC_WRREQ_CREDIT(0x10) | AMDGPU_IH_MC_WR_CLEAN_CNT(0x10) | AMDGPU_IH_MC_VMID(0);
    if (cfg->msi) {
        ih_cntl |= AMDGPU_IH_RPTR_REARM;
    }
    amdgpu_prog_wr(p, AMDGPU_IH_CNTL, ih_cntl);

    /* `si_disable_interrupt_state` (si.c 5928-5956), but for the hot-plug
     * pins and the DAC, which are the display's and left as they are. */
    amdgpu_prog_wr(p, AMDGPU_CP_INT_CNTL_RING0,
                   cfg->cp_int_cntl_ring0 & (AMDGPU_CNTX_BUSY_INT_ENABLE | AMDGPU_CNTX_EMPTY_INT_ENABLE));
    amdgpu_prog_wr(p, AMDGPU_CP_INT_CNTL_RING1, 0);
    amdgpu_prog_wr(p, AMDGPU_CP_INT_CNTL_RING2, 0);
    amdgpu_prog_wr(p, AMDGPU_DMA_CNTL, cfg->dma_cntl[0] & ~AMDGPU_DMA_TRAP_ENABLE);
    amdgpu_prog_wr(p, AMDGPU_DMA_CNTL + AMDGPU_DMA1_OFFSET, cfg->dma_cntl[1] & ~AMDGPU_DMA_TRAP_ENABLE);
    amdgpu_prog_wr(p, AMDGPU_GRBM_INT_CNTL, 0);
    amdgpu_prog_wr(p, AMDGPU_SRBM_INT_CNTL, 0);
    for (k = 0; k < cfg->crtcs; k++) {
        amdgpu_prog_wr(p, AMDGPU_INT_MASK + amdgpu_crtc_offset(k), 0);
    }
    for (k = 0; k < cfg->crtcs; k++) {
        amdgpu_prog_wr(p, AMDGPU_GRPH_INT_CONTROL + amdgpu_crtc_offset(k), 0);
    }

    /* `si_ih_enable_interrupts` (35-45): IH_CNTL, then the ring. */
    amdgpu_prog_wr(p, AMDGPU_IH_CNTL, ih_cntl | AMDGPU_IH_ENABLE_INTR);
    amdgpu_prog_wr(p, AMDGPU_IH_RB_CNTL, rb_cntl | AMDGPU_IH_RB_ENABLE);

    return !p->overflow;
}

void amdgpu_ih_decode(const uint32_t dw[4], struct amdgpu_iv *out)
{
    out->src_id = dw[0] & 0xffu;
    out->src_data = dw[1] & 0xfffffffu;
    out->ring_id = dw[2] & 0xffu;
    out->vmid = (dw[2] >> 8) & 0xffu;
}

uint32_t amdgpu_ih_wptr(uint32_t raw, uint32_t ring_bytes, bool *overflow)
{
    *overflow = (raw & AMDGPU_IH_RB_OVERFLOW) != 0;
    raw &= ~AMDGPU_IH_RB_OVERFLOW;
    return raw & (ring_bytes - 1u);
}

uint32_t amdgpu_ih_next(uint32_t rptr, uint32_t ring_bytes)
{
    return (rptr + AMDGPU_IV_BYTES) & (ring_bytes - 1u);
}

const char *amdgpu_ih_source_name(uint32_t src_id)
{
    switch (src_id) {
    case 1: case 2: case 3: case 4: case 5: case 6:
        return "a vertical blank";
    case AMDGPU_SRC_VM_FAULT:
    case AMDGPU_SRC_VM_FAULT2:
        return "a VM fault";
    case AMDGPU_SRC_CP_RING0:
        return "the CP's ring 0";
    case AMDGPU_SRC_CP_EOP:
        return "a CP end of pipe";
    case AMDGPU_SRC_DMA0_TRAP:
        return "DMA 0's trap";
    case AMDGPU_SRC_DMA1_TRAP:
        return "DMA 1's trap";
    case 230: case 231:
        return "the thermal sensor";
    case AMDGPU_SRC_GUI_IDLE:
        return "the GUI going idle";
    default:
        return NULL;
    }
}
